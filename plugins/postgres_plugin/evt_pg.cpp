/**
 *  @file
 *  @copyright defined in evt/LICENSE.txt
 */
#include <evt/postgres_plugin/evt_pg.hpp>

#include <mutex>
#define FMT_STRING_ALIAS 1
#include <fmt/format.h>
#include <libpq-fe.h>
#include <boost/lexical_cast.hpp>
#include <fc/io/json.hpp>
#include <evt/chain/block_header.hpp>
#include <evt/chain/exceptions.hpp>
#include <evt/chain/contracts/abi_serializer.hpp>
#include <evt/postgres_plugin/copy_context.hpp>
#include <evt/postgres_plugin/trx_context.hpp>

namespace evt {

static auto pg_version = "1.0.0";

namespace __internal {

#define PREPARE_SQL_ONCE(name, sql) \
    static std::once_flag __##name##_flag; \
    std::call_once(__##name##_flag, [&] { \
        auto r = PQprepare(conn_, #name, sql, 0, NULL); \
        EVT_ASSERT(PQresultStatus(r) == PGRES_COMMAND_OK, chain::postgres_exec_exception, "Prepare sql failed, sql: ${s}, detail: ${d}", ("s",sql)("d",PQerrorMessage(conn_))); \
        PQclear(r); \
    });

auto create_stats_table = R"sql(CREATE TABLE IF NOT EXISTS public.stats
                                (
                                    key         character varying(21)    NOT NULL,
                                    value       character varying(64)    NOT NULL,
                                    created_at  timestamp with time zone NOT NULL DEFAULT now(),
                                    updated_at  timestamp with time zone NOT NULL DEFAULT now(),
                                    CONSTRAINT  stats_pkey PRIMARY KEY (key)
                                )
                                WITH (
                                    OIDS = FALSE
                                )
                                TABLESPACE pg_default;)sql";

auto create_blocks_table = R"sql(CREATE TABLE IF NOT EXISTS public.blocks
                                 (
                                     block_id        character(64)            NOT NULL,
                                     block_num       integer                  NOT NULL,
                                     prev_block_id   character(64)            NOT NULL,
                                     timestamp       timestamp with time zone NOT NULL,
                                     trx_merkle_root character(64)            NOT NULL,
                                     trx_count       integer                  NOT NULL,
                                     producer        character varying(21)    NOT NULL,
                                     pending         boolean                  NOT NULL DEFAULT true,
                                     created_at      timestamp with time zone NOT NULL DEFAULT now()
                                 )
                                 WITH (
                                     OIDS = FALSE
                                 )
                                 TABLESPACE pg_default;
 
                                 CREATE INDEX IF NOT EXISTS block_id_index
                                     ON public.blocks USING btree
                                     (block_id)
                                     TABLESPACE pg_default;
 
                                 CREATE INDEX IF NOT EXISTS block_num_index
                                     ON public.blocks USING btree
                                     (block_num)
                                     TABLESPACE pg_default;)sql";

auto create_trxs_table = R"sql(CREATE TABLE IF NOT EXISTS public.transactions
                               (
                                   trx_id        character(64)            NOT NULL,
                                   seq_num       integer                  NOT NULL,
                                   block_id      character(64)            NOT NULL,
                                   block_num     integer                  NOT NULL,
                                   action_count  integer                  NOT NULL,
                                   timestamp     timestamp with time zone NOT NULL,
                                   expiration    timestamp with time zone NOT NULL,
                                   max_charge    integer                  NOT NULL,
                                   payer         character(53)            NOT NULL,
                                   pending       boolean                  NOT NULL DEFAULT true,
                                   type          character varying(7)     NOT NULL,
                                   status        character varying(9)     NOT NULL,
                                   signatures    character(120)[]         NOT NULL,
                                   keys          character(53)[]          NOT NULL,
                                   elapsed       integer                  NOT NULL,
                                   charge        integer                  NOT NULL,
                                   suspend_name  character varying(21),
                                   created_at    timestamp with time zone NOT NULL DEFAULT now()
                               )
                               WITH (
                                   OIDS = FALSE
                               )
                               TABLESPACE pg_default;
                               CREATE INDEX IF NOT EXISTS block_num_index
                                   ON public.transactions USING btree
                                   (block_num)
                                   TABLESPACE pg_default;)sql";

auto create_actions_table = R"sql(CREATE TABLE IF NOT EXISTS public.actions
                                  (
                                      block_id   character(64)            NOT NULL,
                                      block_num  integer                  NOT NULL,
                                      trx_id     character varying(64)    NOT NULL,
                                      seq_num    integer                  NOT NULL,
                                      name       character varying(13)    NOT NULL,
                                      domain     character varying(21)    NOT NULL,
                                      key        character varying(21)    NOT NULL,
                                      data       jsonb                    NOT NULL,
                                      created_at timestamp with time zone NOT NULL DEFAULT now()
                                  )
                                  WITH (
                                      OIDS = FALSE
                                  )
                                  TABLESPACE pg_default;
                                  CREATE INDEX IF NOT EXISTS trx_id_index
                                      ON public.actions USING btree
                                      (trx_id)
                                      TABLESPACE pg_default;)sql";

auto create_metas_table = R"sql(CREATE SEQUENCE IF NOT EXISTS metas_id_seq;
                                CREATE TABLE IF NOT EXISTS metas
                                (
                                    id         integer                   NOT NULL  DEFAULT nextval('metas_id_seq'),
                                    key        character varying(21)     NOT NULL,
                                    value      text                      NOT NULL,
                                    creator    character varying(57)     NOT NULL,
                                    created_at timestamp with time zone  NOT NULL  DEFAULT now(),
                                    CONSTRAINT metas_pkey PRIMARY KEY (id)
                                )
                                WITH (
                                    OIDS = FALSE
                                )
                                TABLESPACE pg_default;)sql";

auto create_domains_table = R"sql(CREATE TABLE IF NOT EXISTS public.domains
                                  (
                                      name       character varying(21)       NOT NULL,
                                      creator    character(53)               NOT NULL,
                                      issue      jsonb                       NOT NULL,
                                      transfer   jsonb                       NOT NULL,
                                      manage     jsonb                       NOT NULL,
                                      metas      integer[]                   NOT NULL,
                                      created_at timestamp with time zone    NOT NULL  DEFAULT now(),
                                      CONSTRAINT domains_pkey PRIMARY KEY (name)
                                  )
                                  WITH (
                                      OIDS = FALSE
                                  )
                                  TABLESPACE pg_default;
                                  CREATE INDEX IF NOT EXISTS creator_index
                                      ON public.domains USING btree
                                      (creator)
                                      TABLESPACE pg_default;)sql";
 
auto create_tokens_table = R"sql(CREATE TABLE IF NOT EXISTS public.tokens
                                 (
                                     id         character varying(42)       NOT NULL,
                                     domain     character varying(21)       NOT NULL,
                                     name       character varying(21)       NOT NULL,
                                     owner      character(53)[]             NOT NULL,
                                     metas      integer[]                   NOT NULL,
                                     created_at timestamp with time zone    NOT NULL  DEFAULT now(),
                                     CONSTRAINT tokens_pkey PRIMARY KEY (id)
                                 )
                                 WITH (
                                     OIDS = FALSE
                                 )
                                 TABLESPACE pg_default;
                                 CREATE INDEX IF NOT EXISTS owner_index
                                     ON public.tokens USING btree
                                     (owner)
                                     TABLESPACE pg_default;)sql";

auto create_groups_table = R"sql(CREATE TABLE IF NOT EXISTS public.groups
                                 (
                                     name       character varying(21)       NOT NULL,
                                     key        character(53)               NOT NULL,
                                     def        jsonb                       NOT NULL,
                                     metas      integer[]                   NOT NULL,
                                     created_at timestamp with time zone    NOT NULL  DEFAULT now(),
                                     CONSTRAINT groups_pkey PRIMARY KEY (name)
                                 )
                                 WITH (
                                     OIDS = FALSE
                                 )
                                 TABLESPACE pg_default;
                                 CREATE INDEX IF NOT EXISTS creator_index
                                     ON public.groups USING btree
                                     (key)
                                     TABLESPACE pg_default;)sql";

auto create_fungibles_table = R"sql(CREATE TABLE IF NOT EXISTS public.fungibles
                                    (
                                        name       character varying(21)       NOT NULL,
                                        sym_name   character varying(21)       NOT NULL,
                                        sym        character varying(21)       NOT NULL,
                                        sym_id     bigint                      NOT NULL,
                                        creator    character(53)               NOT NULL,
                                        issue      jsonb                       NOT NULL,
                                        manage     jsonb                       NOT NULL,
                                        metas      integer[]                   NOT NULL,
                                        created_at timestamp with time zone    NOT NULL  DEFAULT now(),
                                        CONSTRAINT fungibles_pkey PRIMARY KEY (sym_id)
                                    )
                                    WITH (
                                        OIDS = FALSE
                                    )
                                    TABLESPACE pg_default;
                                    CREATE INDEX IF NOT EXISTS creator_index
                                        ON public.fungibles USING btree
                                        (creator)
                                        TABLESPACE pg_default;)sql";

}  // namespace __internal

int
pg::connect(const std::string& conn) {
    conn_ = PQconnectdb(conn.c_str());

    auto status = PQstatus(conn_);
    EVT_ASSERT(status == CONNECTION_OK, chain::postgres_connection_exception, "Connect failed");

    return PG_OK;
}

int
pg::close() {
    FC_ASSERT(conn_);
    PQfinish(conn_);
    conn_ = nullptr;

    return PG_OK;
}

int
pg::create_db(const std::string& db) {
    auto sql = R"sql(CREATE DATABASE {}
                     WITH
                     ENCODING = 'UTF8'
                     LC_COLLATE = 'C'
                     LC_CTYPE = 'C'
                     CONNECTION LIMIT = -1;)sql";
    auto stmt = fmt::format(sql, db);

    auto r = PQexec(conn_, stmt.c_str());
    EVT_ASSERT(PQresultStatus(r) == PGRES_COMMAND_OK, chain::postgres_exec_exception, "Create database failed, detail: ${s}", ("s",PQerrorMessage(conn_)));

    PQclear(r);
    return PG_OK;
}

int
pg::drop_db(const std::string& db) {
    auto sql = "DROP DATABASE {};";
    auto stmt = fmt::format(sql, db);

    auto r = PQexec(conn_, stmt.c_str());
    EVT_ASSERT(PQresultStatus(r) == PGRES_COMMAND_OK, chain::postgres_exec_exception, "Drop database failed, detail: ${s}", ("s",PQerrorMessage(conn_)));

    PQclear(r);
    return PG_OK;
}

int
pg::exists_db(const std::string& db) {
    auto sql = R"sql(SELECT EXISTS(
                         SELECT datname
                         FROM pg_catalog.pg_database WHERE datname = '{}'
                     );)sql";
    auto stmt = fmt::format(sql, db);

    auto r = PQexec(conn_, stmt.c_str());
    EVT_ASSERT(PQresultStatus(r) == PGRES_TUPLES_OK, chain::postgres_exec_exception, "Check if database existed failed, detail: ${s}", ("s",PQerrorMessage(conn_)));

    auto v = PQgetvalue(r, 0, 0);
    if(strcmp(v, "t") == 0) {
        PQclear(r);
        return PG_OK;
    }
    else {
        PQclear(r);
        return PG_FAIL;
    }
}

int
pg::is_table_empty(const std::string& table) {
    auto stmt = fmt::format("SELECT block_id FROM {} LIMIT 1;", table);

    auto r = PQexec(conn_, stmt.c_str());
    EVT_ASSERT(PQresultStatus(r) == PGRES_TUPLES_OK, chain::postgres_exec_exception, "Get one block id failed, detail: ${s}", ("s",PQerrorMessage(conn_)));

    if(PQntuples(r) == 0) {
        PQclear(r);
        return PG_OK;
    }

    PQclear(r);
    return PG_FAIL;
}

int
pg::drop_table(const std::string& table) {
    auto sql = "DROP TABLE IF EXISTS {};";
    auto stmt = fmt::format(sql, table);

    auto r = PQexec(conn_, stmt.c_str());
    EVT_ASSERT(PQresultStatus(r) == PGRES_COMMAND_OK, chain::postgres_exec_exception, "Drop table failed, detail: ${s}", ("s",PQerrorMessage(conn_)));

    PQclear(r);
    return PG_OK;
}

int
pg::drop_sequence(const std::string& seq) {
    auto sql = "DROP SEQUENCE IF EXISTS {};";
    auto stmt = fmt::format(sql, seq);

    auto r = PQexec(conn_, stmt.c_str());
    EVT_ASSERT(PQresultStatus(r) == PGRES_COMMAND_OK, chain::postgres_exec_exception, "Drop sequence failed, detail: ${s}", ("s",PQerrorMessage(conn_)));

    PQclear(r);
    return PG_OK;
}

int
pg::drop_all_tables() {
    drop_table("stats");
    drop_table("blocks");
    drop_table("transactions");
    drop_table("metas");
    drop_table("actions");
    drop_table("domains");
    drop_table("tokens");
    drop_table("groups");
    drop_table("fungibles");

    return PG_OK;
}

int
pg::drop_all_sequences() {
    drop_sequence("metas_id_seq");

    return PG_OK;
}

int
pg::prepare_tables() {
    using namespace __internal;

    const char* stmts[] = {
        create_stats_table,
        create_blocks_table,
        create_trxs_table,
        create_metas_table,
        create_actions_table,
        create_domains_table,
        create_tokens_table,
        create_groups_table,
        create_fungibles_table
    };
    for(auto stmt : stmts) {
        auto r = PQexec(conn_, stmt);
        EVT_ASSERT(PQresultStatus(r) == PGRES_COMMAND_OK, chain::postgres_exec_exception, "Create table failed, detail: ${s}", ("s",PQerrorMessage(conn_)));

        PQclear(r);
    }
    return PG_OK;
}

int
pg::prepare_stats() {
    auto tctx = new_trx_context();
    add_stat(tctx, "version", pg_version);
    add_stat(tctx, "last_sync_block_id", "");

    tctx.commit();
    return PG_OK;
}

int
pg::check_version() {
    auto cur_ver = std::string();
    if(!read_stat("version", cur_ver)) {
        EVT_THROW(chain::postgres_version_exception, "Version information doesn't exist in current database");
    }
    EVT_ASSERT(cur_ver >= pg_version, chain::postgres_version_exception, "Version of current postgres database is obsolete, cur: ${c}, latest: ${l}", ("c",cur_ver)("l",pg_version));
    return PG_OK;
}

int
pg::check_last_sync_block() {
    auto sync_block_id = std::string();
    if(!read_stat("last_sync_block_id", sync_block_id)) {
        EVT_THROW(chain::postgres_sync_exception, "Last sync block id doesn't exist in current database");
    }
    auto last_block_id = std::string();
    if(get_latest_block_id(last_block_id)) {
        EVT_ASSERT(sync_block_id == last_block_id, chain::postgres_sync_exception, "Sync block and latest block are not match, sync is ${s}, latest is ${l}", ("s",sync_block_id)("l",last_block_id));
        last_sync_block_id_ = last_block_id;
        return PG_OK;
    }
    EVT_THROW(chain::postgres_sync_exception, "Cannot get latest block id");
}

copy_context
pg::new_copy_context() {
    return copy_context(*this);
}

int
pg::block_copy_to(const std::string& table, const std::string& data) {
    auto stmt = fmt::format("COPY {} FROM STDIN;", table);

    auto r = PQexec(conn_, stmt.c_str());
    EVT_ASSERT(PQresultStatus(r) == PGRES_COPY_IN, chain::postgres_exec_exception, "Not expected COPY response, detail: ${s}", ("s",PQerrorMessage(conn_)));
    PQclear(r);

    auto nr = PQputCopyData(conn_, data.data(), (int)data.size());
    EVT_ASSERT(nr == 1, chain::postgres_exec_exception, "Put data into COPY stream failed, detail: ${s}", ("s",PQerrorMessage(conn_)));

    auto nr2 = PQputCopyEnd(conn_, NULL);
    EVT_ASSERT(nr == 1, chain::postgres_exec_exception, "Close data into COPY stream failed, detail: ${s}", ("s",PQerrorMessage(conn_)));

    auto r2 = PQgetResult(conn_);
    EVT_ASSERT(PQresultStatus(r2) == PGRES_COMMAND_OK, chain::postgres_exec_exception, "Execute COPY command failed, detail: ${s}", ("s",PQerrorMessage(conn_)));
    PQclear(r2);

    return PG_OK;
}

void
pg::commit_copy_context(copy_context& cctx) {
    if(cctx.blocks_copy_.size() > 0) {
        block_copy_to("blocks", fmt::to_string(cctx.blocks_copy_));
    }
    if(cctx.trxs_copy_.size() > 0) {
        block_copy_to("transactions", fmt::to_string(cctx.trxs_copy_));
    }
    if(cctx.actions_copy_.size() > 0) {
        block_copy_to("actions", fmt::to_string(cctx.actions_copy_));
    }
}

trx_context
pg::new_trx_context() {
    return trx_context(*this);
}

void
pg::commit_trx_context(trx_context& tctx) {
    if(tctx.trx_buf_.size() == 0) {
        return;
    }

    auto stmts = fmt::to_string(tctx.trx_buf_);

    auto r = PQexec(conn_, stmts.c_str());
    auto s = PQresultStatus(r);
    EVT_ASSERT(PQresultStatus(r) == PGRES_COMMAND_OK, chain::postgres_exec_exception, "Commit transactions failed, detail: ${s}", ("s",PQerrorMessage(conn_)));

    PQclear(r);
}

int
pg::add_block(add_context& actx, const block_ptr block) {
    fmt::format_to(actx.cctx.blocks_copy_,
        fmt("{}\t{:d}\t{}\t{}\t{}\t{:d}\t{}\tf\tnow\n"),
        actx.block_id,
        actx.block_num,
        block->header.previous.str(),
        actx.ts,
        block->header.transaction_mroot.str(),
        block->block->transactions.size(),
        (std::string)block->header.producer
        );
    return PG_OK;
}

int
pg::add_trx(add_context& actx, const trx_recept_t& trx, const trx_t& strx, int seq_num, int elapsed, int charge) {
    auto& cctx = actx.cctx;

    fmt::format_to(cctx.trxs_copy_,
        fmt("{}\t{:d}\t{}\t{}\t{:d}\t{}\t{}\t{:d}\t{}\tf\t{}\t{}\t"),
        strx.id().str(),
        seq_num,
        actx.block_id,
        actx.block_num,
        (int32_t)strx.actions.size(),
        actx.ts,
        (std::string)strx.expiration,
        (int32_t)strx.max_charge,
        (std::string)strx.payer,
        (std::string)trx.type,
        (std::string)trx.status
        );;

    // signatures
    fmt::format_to(cctx.trxs_copy_, fmt("{{"));
    if(!strx.signatures.empty()) {
        for(auto i = 0u; i < strx.signatures.size() - 1; i++) {
            auto& sig = strx.signatures[i];
            fmt::format_to(cctx.trxs_copy_, fmt("\"{}\","), (std::string)sig);
        }
        fmt::format_to(cctx.trxs_copy_, fmt("\"{}\""), (std::string)strx.signatures[strx.signatures.size()-1]);
    }
    fmt::format_to(cctx.trxs_copy_, fmt("}}\t"));

    // keys
    fmt::format_to(cctx.trxs_copy_, fmt("{{"));
    if(!strx.signatures.empty()) {
        auto keys = strx.get_signature_keys(actx.chain_id);
        for(auto i = 0u; i < keys.size(); i++) {
            auto& key = *keys.nth(i);
            fmt::format_to(cctx.trxs_copy_, fmt("\"{}\","), (std::string)key);
        }
        fmt::format_to(cctx.trxs_copy_, fmt("\"{}\""), (std::string)*keys.nth(keys.size()-1));
    }
    fmt::format_to(cctx.trxs_copy_, fmt("}}\t"));

    // traces
    fmt::format_to(cctx.trxs_copy_, fmt("{}\t{}\t"), elapsed, charge);

    // extenscions
    auto has_ext = 0;
    for(auto& ext : strx.transaction_extensions) {
        if(std::get<0>(ext) == (uint16_t)chain::transaction_ext::suspend_name) {
            auto& v    = std::get<1>(ext);
            auto  name = std::string(v.cbegin(), v.cend());

            fmt::format_to(cctx.trxs_copy_, fmt("{}\t"), name);
            has_ext = 1;
            break;
        }
    }

    if(has_ext) {
        fmt::format_to(cctx.trxs_copy_, fmt("now\n"));
    }
    else {
        fmt::format_to(cctx.trxs_copy_, fmt("\\N\tnow\n"));
    }

    return PG_OK;
}

int
pg::add_action(add_context& actx, const action_t& act, const std::string& trx_id, int seq_num) {
    auto data = actx.abi.binary_to_variant(actx.abi.get_action_type(act.name), act.data);

    fmt::format_to(actx.cctx.actions_copy_,
        fmt("{}\t{:d}\t{}\t{:d}\t{}\t{}\t{}\t{}\tnow\n"),
        actx.block_id,
        actx.block_num,
        trx_id,
        seq_num,
        act.name.to_string(),
        act.domain.to_string(),
        act.key.to_string(),
        fc::json::to_string(data)
        );

    return PG_OK;
}

int
pg::get_latest_block_id(std::string& block_id) {
    PREPARE_SQL_ONCE(glb_plan, "SELECT block_id FROM blocks ORDER BY block_num DESC LIMIT 1;");

    auto r = PQexecPrepared(conn_, "glb_plan", 0, NULL, NULL, NULL, 0);
    EVT_ASSERT(PQresultStatus(r) == PGRES_TUPLES_OK, chain::postgres_exec_exception, "Get latest block id failed, detail: ${s}", ("s",PQerrorMessage(conn_)));

    if(PQntuples(r) == 0) {
        PQclear(r);
        return PG_FAIL;
    }

    auto v = PQgetvalue(r, 0, 0);
    block_id = v;

    PQclear(r);
    return PG_OK;
}

int
pg::exists_block(const std::string& block_id) {
    PREPARE_SQL_ONCE(eb_plan, "SELECT block_id FROM blocks WHERE block_id = $1;");

    const char* params[] = { block_id.c_str() };

    auto r = PQexecPrepared(conn_, "eb_plan", 1, params, NULL, NULL, 0);
    EVT_ASSERT(PQresultStatus(r) == PGRES_TUPLES_OK, chain::postgres_exec_exception, "Check block existed failed, detail: ${s}", ("s",PQerrorMessage(conn_)));

    if(PQntuples(r) == 0) {
        PQclear(r);
        return PG_FAIL;
    }

    PQclear(r);
    return PG_OK;    
}

int
pg::set_block_irreversible(trx_context& tctx, const std::string& block_id) {
    PREPARE_SQL_ONCE(sbi_plan, "UPDATE blocks SET pending = false WHERE block_id = $1");

    fmt::format_to(tctx.trx_buf_, fmt("EXECUTE sbi_plan('{}');\n"), block_id);
    return PG_OK;
}

int
pg::add_stat(trx_context& tctx, const std::string& key, const std::string& value) {
    PREPARE_SQL_ONCE(as_plan, "INSERT INTO stats VALUES($1, $2, now(), now())");

    fmt::format_to(tctx.trx_buf_, fmt("EXECUTE as_plan('{}','{}');\n"), key, value);
    return PG_OK;
}

int
pg::read_stat(const std::string& key, std::string& value) {
    PREPARE_SQL_ONCE(rs_plan, "SELECT value FROM stats WHERE key = $1");

    const char* params[] = { key.c_str() };
    auto r = PQexecPrepared(conn_, "rs_plan", 1, params, NULL, NULL, 0);
    EVT_ASSERT(PQresultStatus(r) == PGRES_TUPLES_OK, chain::postgres_exec_exception, "Get stat value failed, detail: ${s}", ("s",PQerrorMessage(conn_)));

    if(PQntuples(r) == 0) {
        PQclear(r);
        return PG_FAIL;
    }

    auto v = PQgetvalue(r, 0, 0);
    value = v;

    PQclear(r);
    return PG_OK;
}

int
pg::upd_stat(trx_context& tctx, const std::string& key, const std::string& value) {
    PREPARE_SQL_ONCE(us_plan, "UPDATE stats SET value = $1 WHERE key = $2");

    fmt::format_to(tctx.trx_buf_, fmt("EXECUTE us_plan('{}','{}');\n"), value, key);
    return PG_OK;
}

int
pg::add_domain(trx_context& tctx, const newdomain& nd) {
    PREPARE_SQL_ONCE(nd_plan, "INSERT INTO domains VALUES($1, $2, $3, $4, $5, '{}', now());");

    fc::variant issue, transfer, manage;
    fc::to_variant(nd.issue, issue);
    fc::to_variant(nd.transfer, transfer);
    fc::to_variant(nd.manage, manage);

    fmt::format_to(tctx.trx_buf_,
        fmt("EXECUTE nd_plan('{}','{}','{}','{}','{}');\n"),
        (std::string)nd.name,
        (std::string)nd.creator,
        fc::json::to_string(issue),
        fc::json::to_string(transfer),
        fc::json::to_string(manage)
        );

    return PG_OK;
}

int
pg::upd_domain(trx_context& tctx, const updatedomain& ud) {
    PREPARE_SQL_ONCE(ud_plan, "UPDATE domains SET(issue, transfer, manage) = ($1, $2, $3) WHERE name = $4;");

    std::string i = "issue", t = "transfer", m = "manage";
    if(ud.issue.valid()) {
        fc::variant u;
        fc::to_variant(*ud.issue, u);
        i = fc::json::to_string(u);
    }
    if(ud.transfer.valid()) {
        fc::variant u;
        fc::to_variant(*ud.transfer, u);
        t = fc::json::to_string(u);
    }
    if(ud.manage.valid()) {
        fc::variant u;
        fc::to_variant(*ud.manage, u);
        m = fc::json::to_string(u);
    }

    fmt::format_to(tctx.trx_buf_, fmt("EXECUTE up_plan('{}','{}','{}', '{}');\n"), i, t, m, (std::string)ud.name);

    return PG_OK;
}

int
pg::add_tokens(trx_context& tctx, const issuetoken& it) {
    PREPARE_SQL_ONCE(it_plan, "INSERT INTO tokens VALUES($1, $2, $3, $4, '{}', now());");

    // cache owners
    auto owners_buf = fmt::memory_buffer();
    fmt::format_to(owners_buf, fmt("{{"));
    if(!it.owner.empty()) {
        for(auto i = 0u; i < it.owner.size() - 1; i++) {
            fmt::format_to(owners_buf, fmt("\"{}\","), (std::string)it.owner[i]);
        }
        fmt::format_to(owners_buf, fmt("\"{}\""), (std::string)it.owner[it.owner.size()-1]);
    }
    fmt::format_to(owners_buf, fmt("}}"));

    auto owners = fmt::to_string(owners_buf);
    auto domain = (std::string)it.domain;
    for(auto& name : it.names) {
        fmt::format_to(tctx.trx_buf_,
            fmt("EXECUTE it_plan('{0}:{1}','{0}','{1}','{2}');\n"),
            domain,
            (std::string)name,
            owners
            );
    }
    return PG_OK;
}

int
pg::upd_token(trx_context& tctx, const transfer& tf) {
    PREPARE_SQL_ONCE(tf_plan, "UPDATE tokens SET(owner) = ($1) WHERE id = $2;");

    auto owners_buf = fmt::memory_buffer();
    fmt::format_to(owners_buf, fmt("{{"));
    if(!tf.to.empty()) {
        for(auto i = 0u; i < tf.to.size() - 1; i++) {
            fmt::format_to(owners_buf, fmt("\"{}\","), (std::string)tf.to[i]);
        }
        fmt::format_to(owners_buf, fmt("\"{}\""), (std::string)tf.to[tf.to.size()-1]);
    }
    fmt::format_to(owners_buf, fmt("}}"));

    fmt::format_to(tctx.trx_buf_,
        fmt("EXECUTE tf_plan('{2}','{0}:{1}');"),
        (std::string)tf.domain,
        (std::string)tf.name,
        fmt::to_string(owners_buf)
        );

    return PG_OK;
}

int
pg::del_token(trx_context& tctx, const destroytoken& dt) {
    PREPARE_SQL_ONCE(dt_plan, "UPDATE tokens SET(owner) = ('{\"EVT00000000000000000000000000000000000000000000000000\"}') WHERE id = $1;");

    fmt::format_to(tctx.trx_buf_,
        fmt("EXECUTE dt_plan('{0}:{1}');"),
        (std::string)dt.domain,
        (std::string)dt.name
        );

    return PG_OK;
}

int
pg::add_group(trx_context& tctx, const newgroup& ng) {
    PREPARE_SQL_ONCE(ng_plan, "INSERT INTO groups VALUES($1, $2, $3, '{}', now());");

    fc::variant def;
    fc::to_variant(ng.group, def);

    fmt::format_to(tctx.trx_buf_,
        fmt("EXECUTE ng_plan('{}','{}','{}');\n"),
        (std::string)ng.name,
        (std::string)ng.group.key(),
        fc::json::to_string(def["root"])
        );

    return PG_OK;
}

int
pg::upd_group(trx_context& tctx, const updategroup& ug) {
    PREPARE_SQL_ONCE(ug_plan, "UPDATE groups SET(def) = ($1) WHERE name = $2;");

    fc::variant u;
    fc::to_variant(ug.group, u);

    fmt::format_to(tctx.trx_buf_,
        fmt("EXECUTE ug_plan('{}','{}');"),
        (std::string)ug.name,
        fc::json::to_string(u["root"])
        );

    return PG_OK;
}

int
pg::add_fungible(trx_context& tctx, const newfungible& nf) {
    PREPARE_SQL_ONCE(nf_plan, "INSERT INTO fungibles VALUES($1, $2, $3, $4, $5, $6, $7, '{}', now());");

    fc::variant issue, manage;
    fc::to_variant(nf.issue, issue);
    fc::to_variant(nf.manage, manage);

    fmt::format_to(tctx.trx_buf_,
        fmt("EXECUTE nf_plan('{}','{}','{}',{:d},'{}','{}','{}');\n"),
        (std::string)nf.name,
        (std::string)nf.sym_name,
        (std::string)nf.sym,
        (int64_t)nf.sym.id(),
        (std::string)nf.creator,
        fc::json::to_string(issue),
        fc::json::to_string(manage)
        );

    return PG_OK;
}

int
pg::upd_fungible(trx_context& tctx, const updfungible& uf) {
    PREPARE_SQL_ONCE(uf_plan, "UPDATE fungibles SET(issue, manage) = ($1, $2) WHERE sym_id = $3;");

    std::string i = "issue", m = "manage";
    if(uf.issue.valid()) {
        fc::variant u;
        fc::to_variant(*uf.issue, u);
        i = fc::json::to_string(u);
    }
    if(uf.manage.valid()) {
        fc::variant u;
        fc::to_variant(*uf.manage, u);
        m = fc::json::to_string(u);
    }

    fmt::format_to(tctx.trx_buf_, fmt("EXECUTE uf_plan('{}','{}',{});\n"), i, m, (int64_t)uf.sym_id);

    return PG_OK;
}

int
pg::add_meta(trx_context& tctx, const action_t& act) {
    static std::once_flag __flag;
    std::call_once(__flag, [&] {
        auto prepare = [&](auto name, auto sql) {
            auto r = PQprepare(conn_, name, sql, 0, NULL);
            EVT_ASSERT(PQresultStatus(r) == PGRES_COMMAND_OK, chain::postgres_exec_exception, "Prepare sql failed, sql: ${s}, detail: ${d}", ("s",sql)("d",PQerrorMessage(conn_)));
            PQclear(r);
        };
        prepare("am_plan",  "INSERT INTO metas VALUES(DEFAULT, $1, $2, $3, now());");
        prepare("amd_plan", "UPDATE domains SET metas = array_append(metas, $1) WHERE name = $2;");
        prepare("amt_plan", "UPDATE tokens SET metas = array_append(metas, $1) WHERE id = $2;");
        prepare("amg_plan", "UPDATE groups SET metas = array_append(metas, $1) WHERE name = $2;");
        prepare("amf_plan", "UPDATE fungibles SET metas = array_append(metas, $1) WHERE sym_id = $2;");
    });

    auto& am = act.data_as<const addmeta&>();

    fmt::format_to(tctx.trx_buf_, fmt("EXECUTE am_plan('{}','{}','{}');\n"), (std::string)am.key, am.value, am.creator.to_string());
    if(act.domain == N128(.fungible)) {
        // fungibles
        fmt::format_to(tctx.trx_buf_, fmt("EXECUTE amf_plan(lastval(),{});\n"), boost::lexical_cast<int64_t>((std::string)act.key));   
    }
    else if(act.domain == N128(.group)) {
        // groups
        fmt::format_to(tctx.trx_buf_, fmt("EXECUTE amg_plan(lastval(),'{}');\n"), (std::string)act.key); 
    }
    else if(act.key == N128(.meta)) {
        // domains
        fmt::format_to(tctx.trx_buf_, fmt("EXECUTE amd_plan(lastval(),'{}');\n"), (std::string)act.domain); 
    }
    else {
        // tokens
        fmt::format_to(tctx.trx_buf_, fmt("EXECUTE amt_plan(lastval(),'{}:{}');\n"), (std::string)act.domain, (std::string)act.key); 
    }

    return PG_OK;
}

}  // namepsace evt