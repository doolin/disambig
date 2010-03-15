#include "comp_engine.h"

/*
 * Globals
 */

DB_ENV	 *dbenv;			/* Database environment */
DB	 *db;				/* Primary database */
int	  verbose;			/* Program verbosity */
char	 *progname;			/* Program name */
double Pr_M = 1./100;

//char* on_blocks[] = {"J.SMITH", "D.MORRIS", "J.LEE", NULL};

int main(int argc, char** argv){
    //This should be updated soon to use batch reads and writes
    //Should increase caching performance substantially.
    clock_t start, end, block_start, block_end;
    char ch;
    int big_block = 0;
    int ret, i, freeme, done;
    int custom_block = 0;
    u_int32_t block_num = 0;
    size_t num_digs;
    double likelihood;
    db_recno_t dup_count, max_matches;
    FILE* fp;
    char* filename = "skipped_blocks.txt";
    fp = fopen(filename, "w");
    fclose(fp);

    void* multp;

    u_int32_t matches, num_comps;

    simprof sp;
    char* spkey_buf, **my_block;
    DB_BTREE_STAT *stat;

    DB  *db, *block_db, *sp_db, *rdb, *ldb, *first, *second, *match;
    DBC *cur, *cur_i, *cur_j, *r_cur, *tri_cur, *tag_cur, *ldb_cur;
    DBT mult_data;
    DBT data_i, key_i, pkey_i;
    DBT data_j, key_j, pkey_j;
    DBT data_sp, key_sp;
    DBT key_lik, data_lik;

    memset(&mult_data, 0, sizeof(data_i));

    memset(&data_i, 0, sizeof(data_i));
    memset(&key_i, 0, sizeof(data_i));
    memset(&pkey_i, 0, sizeof(data_i));

    memset(&data_j, 0, sizeof(data_j));
    memset(&key_j, 0, sizeof(data_j));
    memset(&pkey_j, 0, sizeof(data_j));

    memset(&data_sp, 0, sizeof(data_sp));
    memset(&key_sp, 0, sizeof(key_sp));

    DBT_CLEAR(key_lik);
    DBT_CLEAR(data_lik);
    
    sqlite_db_env_open(NULL);
    sqlite_db_primary_open(&db, "primary", DB_BTREE, 32*1024, 0, 0, compare_uint32);
    sqlite_db_secondary_open(db, &block_db, "block_idx", 8*1024, DB_DUPSORT, blocking_callback, compare_uint32);
    sqlite_db_primary_open(&rdb, "ratios", DB_BTREE, 4*1024, 0, 0, NULL);

    ret = db->cursor(db, NULL, &cur, 0);
    ret = block_db->cursor(block_db, NULL, &cur_i, 0);
    ret = rdb->cursor(rdb, NULL, &r_cur, 0);


//Ready the sp_db key buffer
    ret = cur->get(cur, &key_i, &data_i, DB_LAST);
    
    num_digs = (size_t)(log10((double)*(u_int32_t*)key_i.data))+1;
    printf("max_len: %u\n", num_digs);
    spkey_buf = malloc(sizeof(char)*num_digs*2+2);
    
    cur->close(cur);
//Ready to go!
    printf("ready to go!\n");

    memset(&data_i, 0, sizeof(data_i));
    memset(&key_i, 0, sizeof(key_i));
    memset(&pkey_i, 0, sizeof(pkey_i));

    while ((ch = getopt(argc, argv, "b:")) != EOF)
		switch (ch) {
		case 'b':
            key_i.data = optarg;
            key_i.size = strlen(optarg)+1;
            printf("%s, %u\n", key_i.data, key_i.size);
            custom_block = 1;
			break;
        }

    argc -= optind;
	argv += optind;

    if (*argv != NULL)
		return (EXIT_FAILURE);

    //my_block = on_blocks;
    printf("Timing started...\n");
    start = clock();
/*
    while(my_block != NULL){
        printf("block: %s\n", *my_block);
        key_i.data=*my_block;
        key_i.size=strlen(*my_block)+1;
        ++my_block;
      if(custom_block){
            if((ret = cur_i->pget(cur_i, &key_i, &pkey_i, &data_i, DB_SET)))
                return(1);
        }
            
        else
            if((ret = cur_i->pget(cur_i, &key_i, &pkey_i, &data_i, DB_FIRST)))
                return(1);
*/

    while(DB_NOTFOUND !=
      cur_i->pget(cur_i, &key_i, &pkey_i, &data_i,
                  DB_MULTIPLE | (custom_block ? DB_SET : DB_NEXT_NODUP))){//block loop 
        big_block = 0;
        block_start = clock();

        DB_MULTIPLE_INIT(multp, data_i);
        cur_i->dup(cur_i, &tri_cur, DB_POSITION);
        cur_i->dup(cur_i, &tag_cur, DB_POSITION);
        cur_i->count(cur_i, &dup_count, 0);
        if((int)dup_count > 100 || custom_block){
            printf("Big block: %s, %u records\n", (char*)key_i.data, (size_t)dup_count);
            big_block = 1;
    //        continue;
        }
        if(!custom_block && (int)dup_count > 500){
            printf("Block too big: %s, %u\n", (char*)key_i.data, (size_t)dup_count);
            fp = fopen(filename, "a");
            fprintf(fp, "%s, %u\n", (char*)key_i.data, (size_t)dup_count);
            fclose(fp);
            continue;
        }

        Pr_M = MIN(1/5., 10/((double)dup_count));

        if(!(++block_num % 1000) || (block_num < 1000 && !(block_num % 10))) { db->sync(db, 0); printf("%lu blocks processed...\n", (ulong)block_num); }
        if(NULL != has_tag((DbRecord *)data_i.data)){
            //printf("Skipped! %s\n", ((DbRecord *)data_i.data)->Invnum_N);
            continue;
        }
        //else printf("Blocks previously processed: %lu\n", (ulong)block_num); 
        done = 0;

        while(!done){
            cur_i->pget(cur_i, &key_i, &pkey_i, &data_i, DB_SET);
            matches = 0;
            num_comps = 0;
            //printf("again!\n");
            //Compare by block and create likelihoods immediately
            //Only cache in the simprof DB for debugging
            
        /*
            else{
                printf("key_i: %s\n", (char*)key_i.data);
                printf("num_recs: %lu\n", (u_long)dup_count);
            }
        */
            ret = sqlite_db_primary_open(&sp_db, NULL, DB_BTREE, 16*1024, DB_CREATE, 0, NULL);
            //ret = sqlite_db_primary_open(&sp_db, "simprof", DB_BTREE, 16*1024, DB_CREATE, 0, NULL);
            ret = sqlite_db_primary_open(&ldb, NULL, DB_BTREE, 4*1024, DB_CREATE, 0, NULL);
            //ret = sqlite_db_primary_open(&ldb, "lik_db", DB_BTREE, 4*1024, DB_CREATE, 0, NULL);
            ret = sqlite_db_secondary_open(ldb, &first, NULL, 16*1024, DB_DUPSORT, first_index, NULL);
            //ret = sqlite_db_secondary_open(ldb, &first, "first_idx", 16*1024, DB_DUPSORT, first_index, NULL);
            ret = sqlite_db_secondary_open(ldb, &second, NULL, 16*1024, DB_DUPSORT, second_index, NULL);
            //ret = sqlite_db_secondary_open(ldb, &second, "second_idx", 16*1024, DB_DUPSORT, second_index, NULL);
            if(ret)
                printf("DB open problem! %d\n", ret);
            do{
                
                cur_i->dup(cur_i, &cur_j, DB_POSITION);
                while(DB_NOTFOUND != 
                  cur_j->pget(cur_j, &key_j, &pkey_j, &data_j, DB_NEXT_DUP)){
                    sprintf(spkey_buf, "%lu-%lu", (ulong)*(u_int32_t*)pkey_i.data, (ulong)*(u_int32_t*)pkey_j.data);
                    if(!stop_comp((DbRecord*)data_i.data, (DbRecord*)data_j.data)){
                        ret = compare_records(&data_i, &data_j, &sp);
                        
                        key_sp.data = spkey_buf;
                        key_sp.size = strlen(spkey_buf);
                        data_sp.data = &sp;
                        data_sp.size = sizeof(sp);

                        //DEBUG
                        //ret = sp_db->put(sp_db, NULL, &key_sp, &data_sp, 0);
                        //END DEBUG
                        key_lik.data = data_sp.data;
                        key_lik.size = data_sp.size; 
                        ret = r_cur->get(r_cur, &key_lik, &data_lik, DB_SET);
                        if(ret){
                            printf("simprof missing: ");
                            simprof_dump((simprof*)key_lik.data);
                        }
                        likelihood = 1/(1+(1-Pr_M)/(Pr_M*(*(double*)(data_lik.data))));
                        if(likelihood > 0.5) ++matches;
                    }
                    else
                        likelihood = 0;
                    /*
                    if((!strncmp(((DbRecord*)data_i.data)->Firstname, "DAVID", 10) || !strncmp(((DbRecord*)data_j.data)->Firstname, "DAVID", 10)) && likelihood > 0.5){
                        DbRecord_dump((DbRecord*)data_i.data);
                        DbRecord_dump((DbRecord*)data_j.data);
                        printf("likelihood: %g\n", likelihood);
                        printf("ratio: %g\n", *(double*)data_lik.data);
                    }
                    */
                    data_lik.data = &likelihood;
                    data_lik.size = sizeof(double);
         
                    ret = ldb->put(ldb, NULL, &key_sp, &data_lik, 0);
                       
                }
         //       printf("%lu matches in block.\n", (u_long)matches);
            } while(DB_NOTFOUND !=
                cur_i->pget(cur_i, &key_i, &pkey_i, &data_i, DB_NEXT_DUP));
            num_comps = (dup_count*(dup_count-1))/2;
            //printf("matches: %lu\n", (u_long)matches);
            if(fabs(((double)(Pr_M*num_comps-matches))/((double)matches)) > 0.1){
                //printf("Pr_M: %g, Emp: %g\n", Pr_M, (double)matches/(double)num_comps);
                Pr_M = (double)matches/(double)num_comps;//(double)matches/(double)dup_count;
            }
            else{
                //printf("Pr_M: %g, Emp: %g\n", Pr_M, (double)matches/(double)num_comps);
                done = 1;
            }

            if(done && (double)matches/(double)num_comps != 1.0)
                ret = triplet_correct(tri_cur, ldb, first, second);
            //sqlite_db_secondary_open(ldb, &match, "match_idx", 8*1024, DB_DUPSORT, match_index, NULL);
            
            /*
            second->stat(second, NULL, &stat, 0);
            printf("second_idx_nkeys: %lu\n", (u_long)(stat->bt_nkeys));
            printf("int_convert: %d\n", (int)(stat->bt_nkeys));
            if((int)stat->bt_nkeys == 1){
                printf("true!\n");
                ldb->cursor(ldb, NULL, &ldb_cur, 0);
                while(DB_NOTFOUND != ldb_cur->get(ldb_cur, &key_i, &data_i, DB_NEXT)){
                    printf("%s\n", (char*)key_i.data);
                    printf("%g\n", *(double*)data_i.data);
                }
                ldb_cur->close(ldb_cur);
            }
            free(stat);
            */
    //        match->stat(match, NULL, &stat, 0);
    //        printf("match_idx_nkeys: %lu\n", (u_long)(stat->bt_nkeys));
    //        free(stat);
            if(done)
                clump(tag_cur, ldb, first, second, match, db);
            //match->close(match,0);
            first->close(first, 0);
            second->close(second, 0);
            ldb->close(ldb, 0);
            sp_db->close(sp_db, 0);

            //if(dbenv->dbremove(dbenv, NULL, "first_idx", NULL, 0))
            //    printf("complain!\n");
            //if(dbenv->dbremove(dbenv, NULL, "second_idx", NULL, 0))
            //    printf("complain!\n");
            //if(dbenv->dbremove(dbenv, NULL, "match_idx", NULL, 0))
            //    printf("complain!\n");
            //if(dbenv->dbremove(dbenv, NULL, "lik_db", NULL, 0))
            //    printf("complain!\n");
            //if(dbenv->dbremove(dbenv, NULL, "simprof", NULL, 0))
            //    printf("complain!\n");
        }
        block_end = clock();
        if(big_block)
            printf("Block time: %f\n", ((double) (block_end-block_start))/CLOCKS_PER_SEC);
        if(custom_block)
            break;
    }
    free(spkey_buf);
/*
    printf("fname: %d\n", sp.fname);
    printf("street: %d\n", sp.street);
    printf("city: %d\n", sp.city);
    printf("state: %d\n", sp.state);
    printf("country: %d\n", sp.country);
    printf("zipcode: %d\n", sp.zipcode);
    printf("dist: %d\n", sp.dist);
    printf("asg: %d\n", sp.asg);
    printf("firm: %d\n", sp.firm);
*/
    end = clock();

    //ldb->stat_print(ldb, DB_STAT_ALL);
    printf("CPU TIME: %f\n", ((double) (end-start))/CLOCKS_PER_SEC);

    //sp_db->cursor(sp_db, NULL, &cur, 0);

/* 
    key_i.data = "6980-822118";
    key_i.size = 11;

    if(cur->get(cur, &key_i, &data_i, DB_SET)==0){
    printf("fname: %d\n", ((simprof*)data_i.data)->fname);
    printf("street: %d\n", ((simprof*)data_i.data)->street);
    printf("city: %d\n", ((simprof*)data_i.data)->city);
    printf("state: %d\n", ((simprof*)data_i.data)->state);
    printf("country: %d\n", ((simprof*)data_i.data)->country);
    printf("zipcode: %d\n", ((simprof*)data_i.data)->zipcode);
    printf("dist: %d\n", ((simprof*)data_i.data)->dist);
    printf("asg: %d\n", ((simprof*)data_i.data)->asg);
    printf("firm: %d\n", ((simprof*)data_i.data)->firm);}

    key_i.data = "6980-802165";
    key_i.size = 11;

    if(cur->get(cur, &key_i, &data_i, DB_SET)==0){
    printf("fname: %d\n", ((simprof*)data_i.data)->fname);
    printf("street: %d\n", ((simprof*)data_i.data)->street);
    printf("city: %d\n", ((simprof*)data_i.data)->city);
    printf("state: %d\n", ((simprof*)data_i.data)->state);
    printf("country: %d\n", ((simprof*)data_i.data)->country);
    printf("zipcode: %d\n", ((simprof*)data_i.data)->zipcode);
    printf("dist: %d\n", ((simprof*)data_i.data)->dist);
    printf("asg: %d\n", ((simprof*)data_i.data)->asg);
    printf("firm: %d\n", ((simprof*)data_i.data)->firm);}
*/

    //cur->close(cur);
    cur_i->close(cur_i); 
    cur_j->close(cur_j);
    r_cur->close(r_cur);
    //sp_db->close(sp_db, 0);
    rdb->close(rdb, 0);
    //first->close(first, 0);
    //second->close(second, 0);
    //ldb->close(ldb, 0);
    block_db->close(block_db, 0);
    db->close(db,0);
    dbenv->close(dbenv,0);

    return(ret);
}