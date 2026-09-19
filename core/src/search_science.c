#include "internal.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int token_ok(const char *text, size_t max_len) {
    if (!text || !*text || strlen(text) > max_len) return 0;
    for (const unsigned char *p=(const unsigned char *)text; *p; ++p) {
        if (!(isalnum(*p) || *p=='_' || *p=='-' || *p=='.')) return 0;
    }
    return 1;
}

static int sha256_ok(const char *text) {
    if (!text || strlen(text)!=64U) return 0;
    for (const unsigned char *p=(const unsigned char *)text; *p; ++p) {
        if (!isxdigit(*p)) return 0;
    }
    return 1;
}

const char *fvs_search_ranking_version(void) {
    return "search-v1";
}

int fvs_search_eval_record(fvs_ctx *ctx,
                           const char *ranking_version,
                           const char *corpus_sha256,
                           unsigned int mrr_ppm,
                           unsigned int ndcg10_ppm,
                           unsigned int precision10_ppm,
                           unsigned int query_count,
                           char *out,
                           size_t out_size) {
    if (!ctx || !out || !token_ok(ranking_version,48U) || !sha256_ok(corpus_sha256) ||
        mrr_ppm>1000000U || ndcg10_ppm>1000000U || precision10_ppm>1000000U ||
        query_count==0U || query_count>100000U) return FVS_ERR_INVALID;

    char id[37];
    if (fvs_uuid_v4(id)!=FVS_OK) return FVS_ERR_INTERNAL;
    char *ev=fvs_i_escape(ctx,ranking_version);
    char *eh=fvs_i_escape(ctx,corpus_sha256);
    if(!ev||!eh){free(ev);free(eh);return FVS_ERR_INTERNAL;}
    const int needed=snprintf(NULL,0,
        "INSERT INTO search_eval_runs(id,ranking_version,corpus_sha256,mrr_ppm,ndcg10_ppm,precision10_ppm,query_count,created_at) "
        "VALUES('%s','%s','%s',%u,%u,%u,%u,UTC_TIMESTAMP())",
        id,ev,eh,mrr_ppm,ndcg10_ppm,precision10_ppm,query_count);
    if(needed<0){free(ev);free(eh);return FVS_ERR_INTERNAL;}
    char *sql=malloc((size_t)needed+1U);
    if(!sql){free(ev);free(eh);return FVS_ERR_INTERNAL;}
    (void)snprintf(sql,(size_t)needed+1U,
        "INSERT INTO search_eval_runs(id,ranking_version,corpus_sha256,mrr_ppm,ndcg10_ppm,precision10_ppm,query_count,created_at) "
        "VALUES('%s','%s','%s',%u,%u,%u,%u,UTC_TIMESTAMP())",
        id,ev,eh,mrr_ppm,ndcg10_ppm,precision10_ppm,query_count);
    free(ev);free(eh);
    int rc=fvs_i_exec(ctx,sql);
    free(sql);
    if(rc!=FVS_OK)return rc;

    fvs_i_json w;
    fvs_i_json_init(&w,out,out_size);
    fvs_i_json_puts(&w,"{\"eval_id\":");
    fvs_i_json_string(&w,id);
    fvs_i_json_puts(&w,",\"ranking_version\":");
    fvs_i_json_string(&w,ranking_version);
    fvs_i_json_puts(&w,",\"corpus_sha256\":");
    fvs_i_json_string(&w,corpus_sha256);
    fvs_i_json_printf(&w,",\"mrr\":%.6f,\"ndcg10\":%.6f,\"precision10\":%.6f,\"query_count\":%u}",
        (double)mrr_ppm/1000000.0,(double)ndcg10_ppm/1000000.0,(double)precision10_ppm/1000000.0,query_count);
    return fvs_i_json_result(ctx,&w);
}

int fvs_search_quality_dashboard(fvs_ctx *ctx,unsigned int days,char *out,size_t out_size) {
    if(!ctx||!out||days<1U||days>730U)return FVS_ERR_INVALID;

    MYSQL_RES *res=NULL;
    int rc=fvs_i_queryf(ctx,&res,
        "SELECT COUNT(*),COALESCE(SUM(result_count=0),0),COUNT(DISTINCT NULLIF(query_text,'')) "
        "FROM commerce_events WHERE event_type='search' AND created_at>=TIMESTAMPADD(DAY,-%u,UTC_TIMESTAMP())",days);
    if(rc!=FVS_OK)return rc;
    MYSQL_ROW row=mysql_fetch_row(res);
    const unsigned long long searches=row&&row[0]?strtoull(row[0],NULL,10):0ULL;
    const unsigned long long zero=row&&row[1]?strtoull(row[1],NULL,10):0ULL;
    const unsigned long long unique_queries=row&&row[2]?strtoull(row[2],NULL,10):0ULL;
    mysql_free_result(res);

    fvs_i_json w;
    fvs_i_json_init(&w,out,out_size);
    fvs_i_json_printf(&w,
        "{\"days\":%u,\"searches\":%llu,\"zero_results\":%llu,\"zero_result_rate\":%.6f,\"unique_queries\":%llu,\"ranking_versions\":[",
        days,searches,zero,searches?(double)zero/(double)searches:0.0,unique_queries);

    rc=fvs_i_queryf(ctx,&res,
        "SELECT COALESCE(JSON_UNQUOTE(JSON_EXTRACT(metadata_json,'$.ranking_version')),'unknown'),COUNT(*),COALESCE(SUM(result_count=0),0) "
        "FROM commerce_events WHERE event_type='search' AND created_at>=TIMESTAMPADD(DAY,-%u,UTC_TIMESTAMP()) "
        "GROUP BY 1 ORDER BY COUNT(*) DESC,1",days);
    if(rc!=FVS_OK)return rc;
    int first=1;
    while((row=mysql_fetch_row(res))!=NULL){
        if(!first)fvs_i_json_puts(&w,",");
        first=0;
        const unsigned long long n=row[1]?strtoull(row[1],NULL,10):0ULL;
        const unsigned long long z=row[2]?strtoull(row[2],NULL,10):0ULL;
        fvs_i_json_puts(&w,"{\"version\":");
        fvs_i_json_string(&w,row[0]?row[0]:"unknown");
        fvs_i_json_printf(&w,",\"searches\":%llu,\"zero_results\":%llu,\"zero_result_rate\":%.6f}",n,z,n?(double)z/(double)n:0.0);
    }
    mysql_free_result(res);

    fvs_i_json_puts(&w,"],\"top_zero_queries\":[");
    rc=fvs_i_queryf(ctx,&res,
        "SELECT query_text,COUNT(*) FROM commerce_events "
        "WHERE event_type='search' AND result_count=0 AND query_text IS NOT NULL AND query_text<>'' "
        "AND created_at>=TIMESTAMPADD(DAY,-%u,UTC_TIMESTAMP()) "
        "GROUP BY query_text ORDER BY COUNT(*) DESC,query_text LIMIT 20",days);
    if(rc!=FVS_OK)return rc;
    first=1;
    while((row=mysql_fetch_row(res))!=NULL){
        if(!first)fvs_i_json_puts(&w,",");
        first=0;
        fvs_i_json_puts(&w,"{\"query\":");
        fvs_i_json_string(&w,row[0]?row[0]:"");
        fvs_i_json_printf(&w,",\"count\":%llu}",row[1]?strtoull(row[1],NULL,10):0ULL);
    }
    mysql_free_result(res);

    fvs_i_json_puts(&w,"],\"last_eval\":");
    rc=fvs_i_queryf(ctx,&res,
        "SELECT ranking_version,corpus_sha256,mrr_ppm,ndcg10_ppm,precision10_ppm,query_count,DATE_FORMAT(created_at,'%%Y-%%m-%%dT%%H:%%i:%%sZ') "
        "FROM search_eval_runs ORDER BY created_at DESC,id DESC LIMIT 1");
    if(rc!=FVS_OK)return rc;
    row=mysql_fetch_row(res);
    if(!row){
        fvs_i_json_puts(&w,"null");
    }else{
        fvs_i_json_puts(&w,"{\"ranking_version\":");
        fvs_i_json_string(&w,row[0]);
        fvs_i_json_puts(&w,",\"corpus_sha256\":");
        fvs_i_json_string(&w,row[1]);
        fvs_i_json_printf(&w,",\"mrr\":%.6f,\"ndcg10\":%.6f,\"precision10\":%.6f,\"query_count\":%u,\"created_at\":",
            row[2]?(double)strtoul(row[2],NULL,10)/1000000.0:0.0,
            row[3]?(double)strtoul(row[3],NULL,10)/1000000.0:0.0,
            row[4]?(double)strtoul(row[4],NULL,10)/1000000.0:0.0,
            row[5]?(unsigned int)strtoul(row[5],NULL,10):0U);
        fvs_i_json_string(&w,row[6]);
        fvs_i_json_puts(&w,"}");
    }
    mysql_free_result(res);
    fvs_i_json_puts(&w,"}");
    return fvs_i_json_result(ctx,&w);
}
