#include "internal.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int recovery_state_valid(const char *state) {
    return state && (!strcmp(state,"active") || !strcmp(state,"provider_error") ||
                     !strcmp(state,"manual_review") || !strcmp(state,"abandoned") ||
                     !strcmp(state,"resolved"));
}

static int reason_valid(const char *reason) {
    if (!reason || !*reason) return 1;
    if (strlen(reason) > 80U) return 0;
    for (const unsigned char *p=(const unsigned char *)reason; *p; ++p) {
        if (!(isalnum(*p) || *p=='_' || *p=='-' || *p=='.')) return 0;
    }
    return 1;
}

int fvs_payment_recovery_mark(fvs_ctx *ctx,const char *attempt_id,const char *state,
                              const char *reason_code,const char *last_error,
                              unsigned int retry_after_seconds) {
    if (!ctx || !fvs_i_valid_uuid(attempt_id) || !recovery_state_valid(state) ||
        !reason_valid(reason_code) || (last_error && strlen(last_error)>1000U) ||
        retry_after_seconds>86400U) return FVS_ERR_INVALID;
    char *es=fvs_i_escape(ctx,state),*er=fvs_i_escape(ctx,reason_code?reason_code:""),
         *ee=fvs_i_escape(ctx,last_error?last_error:"");
    if(!es||!er||!ee){free(es);free(er);free(ee);return FVS_ERR_INTERNAL;}
    const int needed=snprintf(NULL,0,
        "INSERT INTO payment_recovery_cases(attempt_id,state,reason_code,retry_count,next_retry_at,last_error,first_seen_at,last_seen_at,resolved_at) "
        "SELECT id,'%s',NULLIF('%s',''),%u,IF(%u>0,TIMESTAMPADD(SECOND,%u,UTC_TIMESTAMP()),NULL),NULLIF('%s',''),UTC_TIMESTAMP(),UTC_TIMESTAMP(),IF('%s'='resolved',UTC_TIMESTAMP(),NULL) "
        "FROM payment_attempts WHERE id='%s' ON DUPLICATE KEY UPDATE state=VALUES(state),reason_code=VALUES(reason_code),"
        "retry_count=payment_recovery_cases.retry_count+IF(VALUES(state)='provider_error',1,0),"
        "next_retry_at=VALUES(next_retry_at),last_error=VALUES(last_error),last_seen_at=UTC_TIMESTAMP(),"
        "resolved_at=IF(VALUES(state)='resolved',UTC_TIMESTAMP(),NULL)",
        es,er,!strcmp(state,"provider_error")?1U:0U,retry_after_seconds,retry_after_seconds,ee,es,attempt_id);
    if(needed<0){free(es);free(er);free(ee);return FVS_ERR_INTERNAL;}
    char *sql=malloc((size_t)needed+1U);if(!sql){free(es);free(er);free(ee);return FVS_ERR_INTERNAL;}
    (void)snprintf(sql,(size_t)needed+1U,
        "INSERT INTO payment_recovery_cases(attempt_id,state,reason_code,retry_count,next_retry_at,last_error,first_seen_at,last_seen_at,resolved_at) "
        "SELECT id,'%s',NULLIF('%s',''),%u,IF(%u>0,TIMESTAMPADD(SECOND,%u,UTC_TIMESTAMP()),NULL),NULLIF('%s',''),UTC_TIMESTAMP(),UTC_TIMESTAMP(),IF('%s'='resolved',UTC_TIMESTAMP(),NULL) "
        "FROM payment_attempts WHERE id='%s' ON DUPLICATE KEY UPDATE state=VALUES(state),reason_code=VALUES(reason_code),"
        "retry_count=payment_recovery_cases.retry_count+IF(VALUES(state)='provider_error',1,0),"
        "next_retry_at=VALUES(next_retry_at),last_error=VALUES(last_error),last_seen_at=UTC_TIMESTAMP(),"
        "resolved_at=IF(VALUES(state)='resolved',UTC_TIMESTAMP(),NULL)",
        es,er,!strcmp(state,"provider_error")?1U:0U,retry_after_seconds,retry_after_seconds,ee,es,attempt_id);
    free(es);free(er);free(ee);
    int rc=fvs_i_exec(ctx,sql);free(sql);return rc;
}

int fvs_payment_recovery_get(fvs_ctx *ctx,const char *attempt_id,char *out,size_t out_size) {
    if(!ctx||!fvs_i_valid_uuid(attempt_id)||!out)return FVS_ERR_INVALID;
    MYSQL_RES *res=NULL;int rc=fvs_i_queryf(ctx,&res,
        "SELECT r.state,COALESCE(r.reason_code,''),r.retry_count,DATE_FORMAT(r.next_retry_at,'%%Y-%%m-%%dT%%H:%%i:%%sZ'),"
        "COALESCE(r.last_error,''),DATE_FORMAT(r.first_seen_at,'%%Y-%%m-%%dT%%H:%%i:%%sZ'),DATE_FORMAT(r.last_seen_at,'%%Y-%%m-%%dT%%H:%%i:%%sZ'),"
        "DATE_FORMAT(r.resolved_at,'%%Y-%%m-%%dT%%H:%%i:%%sZ'),p.provider,p.status,p.cart_id "
        "FROM payment_recovery_cases r JOIN payment_attempts p ON p.id=r.attempt_id WHERE r.attempt_id='%s'",attempt_id);
    if(rc!=FVS_OK)return rc;MYSQL_ROW row=mysql_fetch_row(res);if(!row){mysql_free_result(res);return FVS_ERR_NOT_FOUND;}
    fvs_i_json w;fvs_i_json_init(&w,out,out_size);fvs_i_json_puts(&w,"{\"attempt_id\":");fvs_i_json_string(&w,attempt_id);
    fvs_i_json_puts(&w,",\"state\":");fvs_i_json_string(&w,row[0]);fvs_i_json_puts(&w,",\"reason_code\":");row[1]&&*row[1]?fvs_i_json_string(&w,row[1]):fvs_i_json_puts(&w,"null");
    fvs_i_json_printf(&w,",\"retry_count\":%u,\"next_retry_at\":",row[2]?(unsigned int)strtoul(row[2],NULL,10):0U);row[3]?fvs_i_json_string(&w,row[3]):fvs_i_json_puts(&w,"null");
    fvs_i_json_puts(&w,",\"last_error\":");row[4]&&*row[4]?fvs_i_json_string(&w,row[4]):fvs_i_json_puts(&w,"null");
    fvs_i_json_puts(&w,",\"first_seen_at\":");fvs_i_json_string(&w,row[5]);fvs_i_json_puts(&w,",\"last_seen_at\":");fvs_i_json_string(&w,row[6]);
    fvs_i_json_puts(&w,",\"resolved_at\":");row[7]?fvs_i_json_string(&w,row[7]):fvs_i_json_puts(&w,"null");
    fvs_i_json_puts(&w,",\"provider\":");fvs_i_json_string(&w,row[8]);fvs_i_json_puts(&w,",\"payment_status\":");fvs_i_json_string(&w,row[9]);fvs_i_json_puts(&w,",\"cart_id\":");fvs_i_json_string(&w,row[10]);fvs_i_json_puts(&w,"}");
    mysql_free_result(res);return fvs_i_json_result(ctx,&w);
}

int fvs_payment_recovery_dashboard(fvs_ctx *ctx,unsigned int days,char *out,size_t out_size) {
    if(!ctx||!out||days<1U||days>730U)return FVS_ERR_INVALID;
    MYSQL_RES *res=NULL;int rc=fvs_i_queryf(ctx,&res,
        "SELECT COUNT(*),COALESCE(SUM(state='active'),0),COALESCE(SUM(state='provider_error'),0),"
        "COALESCE(SUM(state='manual_review'),0),COALESCE(SUM(state='abandoned'),0),COALESCE(SUM(state='resolved'),0),"
        "COALESCE(SUM(state IN ('active','provider_error') AND last_seen_at<TIMESTAMPADD(MINUTE,-15,UTC_TIMESTAMP())),0) "
        "FROM payment_recovery_cases WHERE first_seen_at>=TIMESTAMPADD(DAY,-%u,UTC_TIMESTAMP())",days);
    if(rc!=FVS_OK)return rc;MYSQL_ROW row=mysql_fetch_row(res);
    unsigned long long total=row&&row[0]?strtoull(row[0],NULL,10):0ULL,active=row&&row[1]?strtoull(row[1],NULL,10):0ULL,
        provider_error=row&&row[2]?strtoull(row[2],NULL,10):0ULL,manual=row&&row[3]?strtoull(row[3],NULL,10):0ULL,
        abandoned=row&&row[4]?strtoull(row[4],NULL,10):0ULL,resolved=row&&row[5]?strtoull(row[5],NULL,10):0ULL,
        stale=row&&row[6]?strtoull(row[6],NULL,10):0ULL;mysql_free_result(res);
    fvs_i_json w;fvs_i_json_init(&w,out,out_size);fvs_i_json_printf(&w,
        "{\"days\":%u,\"total\":%llu,\"active\":%llu,\"provider_error\":%llu,\"manual_review\":%llu,\"abandoned\":%llu,\"resolved\":%llu,\"stale_recoverable\":%llu,\"by_provider\":[",
        days,total,active,provider_error,manual,abandoned,resolved,stale);
    rc=fvs_i_queryf(ctx,&res,
        "SELECT p.provider,r.state,COUNT(*) FROM payment_recovery_cases r JOIN payment_attempts p ON p.id=r.attempt_id "
        "WHERE r.first_seen_at>=TIMESTAMPADD(DAY,-%u,UTC_TIMESTAMP()) GROUP BY p.provider,r.state ORDER BY p.provider,r.state",days);
    if(rc!=FVS_OK)return rc;int first=1;while((row=mysql_fetch_row(res))!=NULL){if(!first)fvs_i_json_puts(&w,",");first=0;fvs_i_json_puts(&w,"{\"provider\":");fvs_i_json_string(&w,row[0]);fvs_i_json_puts(&w,",\"state\":");fvs_i_json_string(&w,row[1]);fvs_i_json_printf(&w,",\"count\":%llu}",row[2]?strtoull(row[2],NULL,10):0ULL);}mysql_free_result(res);
    fvs_i_json_puts(&w,"]}");return fvs_i_json_result(ctx,&w);
}
