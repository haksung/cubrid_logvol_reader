<cubrid log structure 찾아내기>
( c 파일로 저장해야 보기 좋아서, c 파일로 저장 )

-------------------------------------------------------------------------------------------------------

[log applier에 글로벌 변수로 선언된 la_Info의 구조체]
	/* Log applier info */
	typedef struct la_info LA_INFO;
	struct la_info
	{
	  /* log info */
	  char log_path[PATH_MAX];
	  char loginf_path[PATH_MAX];
	  LA_ACT_LOG act_log;
	  LA_ARV_LOG arv_log;
	  int last_file_state;
	  unsigned long start_vsize;
	  time_t start_time;

	  /* map info */
	  LOG_LSA final_lsa;		/* last processed log lsa */
	  LOG_LSA committed_lsa;	/* last committed commit log lsa */
	  LOG_LSA committed_rep_lsa;	/* last committed replication log lsa */
	  LOG_LSA last_committed_lsa;	/* last committed commit log lsa
					 * at the beginning of the applylogdb */
	  LOG_LSA last_committed_rep_lsa;	/* last committed replication log lsa
						 * at the beginning of the applylogdb */

	  LA_APPLY **repl_lists;
	  int repl_cnt;			/* the # of elements of repl_lists */
	  int cur_repl;			/* the index of the current repl_lists */
	  int total_rows;		/* the # of rows that were replicated */
	  int prev_total_rows;		/* the previous # of total_rows */
	  time_t log_record_time;	/* time of the last commit log record */
	  LA_COMMIT *commit_head;	/* queue list head */
	  LA_COMMIT *commit_tail;	/* queue list tail */
	  int last_deleted_archive_num;
	  /* last time that one or more archives were deleted */
	  time_t last_time_archive_deleted;

	  /* slave info */
	  char *log_data;
	  char *rec_type;
	  LOG_ZIP *undo_unzip_ptr;
	  LOG_ZIP *redo_unzip_ptr;
	  int apply_state;
	  int max_mem_size;

	  /* master info */
	  LA_CACHE_PB *cache_pb;
	  int cache_buffer_size;
	  bool last_is_end_of_record;
	  bool is_end_of_record;
	  int last_server_state;
	  bool is_role_changed;

	  /* db_ha_apply_info */
	  LOG_LSA append_lsa;		/* append lsa of active log header */
	  LOG_LSA eof_lsa;		/* eof lsa of active log header */
	  LOG_LSA required_lsa;		/* start lsa of the first transaction
					 * to be applied */
	  unsigned long insert_counter;
	  unsigned long update_counter;
	  unsigned long delete_counter;
	  unsigned long schema_counter;
	  unsigned long commit_counter;
	  unsigned long fail_counter;
	  time_t log_commit_time;
	  bool required_lsa_changed;
	  int status;
	  bool is_apply_info_updated;	/* whether catalog is partially updated or not */

	  int num_unflushed_insert;

	  /* file lock */
	  int log_path_lockf_vdes;
	  int db_lockf_vdes;

	  LA_REPL_FILTER repl_filter;
	};

-------------------------------------------------------------------------------------------------------

[구동중에 찍어본 la_Info]

	{
		log_path = "/home/haksung1/CUBRID936/databases/demodb_newTest1", '\000' <repeats 4045 times>, // log 파일들 주소
		loginf_path = "/home/haksung1/CUBRID936/databases/demodb_newTest1/demodb_lginf", '\000' <repeats 4032 times>, // lginf 주소
		act_log = { path = "/home/haksung1/CUBRID936/databases/demodb_newTest1/demodb_lgat", '\000' <repeats 4033 times>, // active 로그 관련
					log_vdes = 120, hdr_page = 0x1e540c0, log_hdr = 0x1e540d0, db_iopagesize = 16384, db_logpagesize = 16384},
		arv_log = {path = '\000' <repeats 4095 times>, log_vdes = -1, hdr_page = 0x0, log_hdr = 0x0, arv_num = 0},
		last_file_state = 2,
		start_vsize = 50188,
		start_time = 1481697920, // applylogdb 유틸리티 시작한 시간
		final_lsa = {pageid = 5140, offset = 2272},
		committed_lsa = { pageid = 4793, offset = 5736},
		committed_rep_lsa = {pageid = 4793, offset = 5656},
		last_committed_lsa = {pageid = 4792, offset = 360}, 
		last_committed_rep_lsa = {pageid = 728, offset = 11552},
		repl_lists = 0x1e59b80,
		repl_cnt = 50,
		cur_repl = 1,
		total_rows = 1,
		prev_total_rows = 1, 
		log_record_time = 1481767898, 
		commit_head = 0x0, commit_tail = 0x0,
		last_deleted_archive_num = -1, last_time_archive_deleted = 1481697920,
		log_data = 0x1e92d40 "\240", rec_type = 0x1e53f30 "\002",
		undo_unzip_ptr = 0x1dfd9b0, redo_unzip_ptr = 0x1e92cb0, 
		apply_state = 2, 
		max_mem_size = 500,
		cache_pb = 0x1e52ef0, cache_buffer_size = 100, 
		last_is_end_of_record = true, is_end_of_record = true,
		last_server_state = 1, is_role_changed = false,
		append_lsa = {pageid = 5140, offset = 1624},
		eof_lsa = {pageid = 5140, offset = 1624},
		required_lsa = {pageid = 5140, offset = 1624},
		insert_counter = 1, update_counter = 0, delete_counter = 0, schema_counter = 0,
		commit_counter = 5, fail_counter = 0, log_commit_time = 1481766779,
		required_lsa_changed = false, status = 0, 
		is_apply_info_updated = false,
		num_unflushed_insert = 0, log_path_lockf_vdes = 119, 
		db_lockf_vdes = 121, 
		repl_filter = {
		list = 0x0, list_size = 0, num_filters = 0, type = REPL_FILTER_NONE}
	}

-------------------------------------------------------------------------------------------------------

[구동중에 찍어본 *la_Info.act_log.log_hdr]

	{
		magic = "CUBRID/LogActive\000\000\000\000\000\000\000\000", dummy = 0,
	  db_creation = 1481595989, db_release = "9.3.6\000\000\000\000\000\000\000\000\000",
	  db_compatibility = 9.19999981, db_iopagesize = 16384, db_logpagesize = 16384, is_shutdown = 1,
	  next_trid = 142002, avg_ntrans = 0, avg_nlocks = 0, npages = 6399, db_charset = 5 '\005',
	  dummy2 = 0 '\000', dummy3 = 0 '\000', dummy4 = 0 '\000', fpageid = 0, append_lsa = {
		pageid = 5206, offset = 15680}, chkpt_lsa = {pageid = 5205, offset = 7616}, nxarv_pageid = 0,
	  nxarv_phy_pageid = 1, nxarv_num = 0, last_arv_num_for_syscrashes = -1,
	  last_deleted_arv_num = -1, bkup_level0_lsa = {pageid = -1, offset = -1}, bkup_level1_lsa = {
		pageid = -1, offset = -1}, bkup_level2_lsa = {pageid = -1, offset = -1},
	  prefix_name = "demodb", '\000' <repeats 11 times>, has_logging_been_skipped = false,
	  reserved_int_1 = -1, reserved_int_2 = -1, perm_status = 0, bkinfo = {{bkup_attime = 0,
		  io_baseln_time = 0, io_bkuptime = 0, ndirty_pages_post_bkup = 0, io_numpages = 0}, {
		  bkup_attime = 0, io_baseln_time = 0, io_bkuptime = 0, ndirty_pages_post_bkup = 0,
		  io_numpages = 0}, {bkup_attime = 0, io_baseln_time = 0, io_bkuptime = 0,
		  ndirty_pages_post_bkup = 0, io_numpages = 0}}, ha_server_state = 1, ha_file_status = 2,
	  eof_lsa = {pageid = 5206, offset = 15680}, smallest_lsa_at_last_chkpt = {pageid = 5205,
		offset = 7616}}

