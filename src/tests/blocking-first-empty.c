// verify that cursor first on an empty tree with a write lock suspends the conflicting threads.

#include "test.h"
#include "toku_pthread.h"

struct my_callback_context {
    DBT key;
    DBT val;
};

#if TOKUDB
static void copy_dbt(DBT *dest, DBT const *src) {
    assert(dest->flags == DB_DBT_REALLOC);
    dest->size = src->size;
    dest->data = toku_xrealloc(dest->data, dest->size);
    memcpy(dest->data, src->data, dest->size);
}

static int blocking_first_callback(DBT const *a UU(), DBT const *b UU(), void *e UU()) {
    DBT const *found_key = a;
    DBT const *found_val = b;
    struct my_callback_context *context = (struct my_callback_context *) e;
    copy_dbt(&context->key, found_key);
    copy_dbt(&context->val, found_val);
    return 0;
}
#endif

static void blocking_first(DB_ENV *db_env, DB *db, uint64_t nrows, long sleeptime) {
    int r;

    struct my_callback_context context;
    context.key = (DBT) { .data = NULL, .size = 0, .flags = DB_DBT_REALLOC };
    context.val = (DBT) { .data = NULL, .size = 0, .flags = DB_DBT_REALLOC };

    for (uint64_t i = 0; i < nrows; i++) {
        DB_TXN *txn = NULL;
        r = db_env->txn_begin(db_env, NULL, &txn, 0); assert(r == 0);

        DBC *cursor = NULL;
        r = db->cursor(db, txn, &cursor, 0); assert(r == 0); // get a write lock on -inf +inf
#if TOKUDB
        r = cursor->c_getf_first(cursor, DB_RMW, blocking_first_callback, &context); assert(r == DB_NOTFOUND);
#else
        r = cursor->c_get(cursor, &context.key, &context.val, DB_FIRST + DB_RMW); assert(r == DB_NOTFOUND);
#endif

        usleep(sleeptime);

        r = cursor->c_close(cursor); assert(r == 0);

        r = txn->commit(txn, 0); assert(r == 0);
        if (verbose)
            printf("%lu %lu\n", toku_pthread_self(), i);
    }

    toku_free(context.key.data);
    toku_free(context.val.data);
}

struct blocking_first_args {
    DB_ENV *db_env;
    DB *db;
    uint64_t nrows;
    long sleeptime;
};

static void *blocking_first_thread(void *arg) {
    struct blocking_first_args *a = (struct blocking_first_args *) arg;
    blocking_first(a->db_env, a->db, a->nrows, a->sleeptime);
    return arg;
}

static void run_test(DB_ENV *db_env, DB *db, int nthreads, uint64_t nrows, long sleeptime) {
    int r;
    toku_pthread_t tids[nthreads];
    struct blocking_first_args a = { db_env, db, nrows, sleeptime };
    for (int i = 0; i < nthreads-1; i++) {
        r = toku_pthread_create(&tids[i], NULL, blocking_first_thread, &a); assert(r == 0);
    }
    blocking_first(db_env, db, nrows, sleeptime);
    for (int i = 0; i < nthreads-1; i++) {
        void *ret;
        r = toku_pthread_join(tids[i], &ret); assert(r == 0);
    }
}

int test_main(int argc, char * const argv[]) {
    uint64_t cachesize = 0;
    uint32_t pagesize = 0;
    uint64_t nrows = 10;
    int nthreads = 2;
    long sleeptime = 100000;
#if defined(USE_TDB)
    char *db_env_dir = "dir." __FILE__ ".tokudb";
#elif defined(USE_BDB)
    char *db_env_dir = "dir." __FILE__ ".bdb";
#else
#error
#endif
    char *db_filename = "test.db";
    int db_env_open_flags = DB_CREATE | DB_PRIVATE | DB_INIT_MPOOL | DB_INIT_TXN | DB_INIT_LOCK | DB_INIT_LOG | DB_THREAD;

    // parse_args(argc, argv);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose++;
            continue;
        }
        if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            if (verbose > 0)
                verbose--;
            continue;
        }
        if (strcmp(argv[i], "--nrows") == 0 && i+1 < argc) {
            nrows = atoll(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--nthreads") == 0 && i+1 < argc) {
            nthreads = atoi(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--sleeptime") == 0 && i+1 < argc) {
            sleeptime = atol(argv[++i]);
            continue;
        }
        assert(0);
    }

    // setup env
    int r;
    char rm_cmd[strlen(db_env_dir) + strlen("rm -rf ") + 1];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", db_env_dir);
    r = system(rm_cmd); assert(r == 0);

    r = toku_os_mkdir(db_env_dir, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH); assert(r == 0);

    DB_ENV *db_env = NULL;
    r = db_env_create(&db_env, 0); assert(r == 0);
    if (cachesize) {
        const u_int64_t gig = 1 << 30;
        r = db_env->set_cachesize(db_env, cachesize / gig, cachesize % gig, 1); assert(r == 0);
    }
    r = db_env->open(db_env, db_env_dir, db_env_open_flags, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH); assert(r == 0);
#if TOKUDB
    r = db_env->set_lock_timeout(db_env, 30 * 1000); assert(r == 0);
#endif

    // create the db
    DB *db = NULL;
    r = db_create(&db, db_env, 0); assert(r == 0);
    if (pagesize) {
        r = db->set_pagesize(db, pagesize); assert(r == 0);
    }
    r = db->open(db, NULL, db_filename, NULL, DB_BTREE, DB_CREATE|DB_AUTO_COMMIT|DB_THREAD, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH); assert(r == 0);

    run_test(db_env, db, nthreads, nrows, sleeptime);

    // close env
    r = db->close(db, 0); assert(r == 0); db = NULL;
    r = db_env->close(db_env, 0); assert(r == 0); db_env = NULL;

    return 0;
}
