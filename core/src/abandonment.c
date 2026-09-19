#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FVS_ABANDONMENT_MAX_SCAN 200U

static int abandonment_counts(fvs_ctx *ctx,unsigned int days,char *out,size_t out_size) {
    MYSQL_RES *res=NULL;
    int rc=fvs_i_queryf(ctx,&res,
        "SELECT COUNT(*),COALESCE(SUM(state='detected'),0),COALESCE(SUM(state='scheduled'),0),"
        "COALESCE(SUM(state='converted'),0),COALESCE(SUM(state='suppressed'),0) "
        "FROM commerce_abandonment_cases WHERE detected_at>=TIMESTAMPADD(DAY,-%u,UTC_TIMESTAMP())",days);
    if(rc!=FVS_OK)return rc;
    MYSQL_ROW row=mysql_fetch_row(res);
    const unsigned long long total=row&&row[0]?strtoull(row[0],NULL,10):0ULL;
    const unsigned long long detected=row&&row[1]?strtoull(row[1],NULL,10):0ULL;
    const unsigned long long scheduled=row&&row[2]?strtoull(row[2],NULL,10):0ULL;
    const unsigned long long converted=row&&row[3]?strtoull(row[3],NULL,10):0ULL;
    const unsigned long long suppressed=row&&row[4]?strtoull(row[4],NULL,10):0ULL;
    mysql_free_result(res);
    fvs_i_json w;
    fvs_i_json_init(&w,out,out_size);
    fvs_i_json_printf(&w,
        "{\"days\":%u,\"total\":%llu,\"detected\":%llu,\"scheduled\":%llu,"
        "\"converted\":%llu,\"suppressed\":%llu,\"conversion_after_detection_rate\":%.6f}",
        days,total,detected,scheduled,converted,suppressed,
        total?(double)converted/(double)total:0.0);
    return fvs_i_json_result(ctx,&w);
}

int fvs_abandonment_scan(fvs_ctx *ctx,unsigned int idle_seconds,unsigned int limit,
                         char *out,size_t out_size) {
    if(!ctx||idle_seconds<300U||idle_seconds>604800U||limit<1U||
       limit>FVS_ABANDONMENT_MAX_SCAN||!out)return FVS_ERR_INVALID;

    int rc=fvs_i_exec(ctx,
        "UPDATE marketing_jobs j "
        "JOIN commerce_abandonment_cases ac ON ac.job_id=j.id "
        "JOIN orders o ON o.payment_attempt_id=ac.attempt_id AND o.status='confirmed' "
        "SET j.status='cancelled',j.updated_at=UTC_TIMESTAMP() "
        "WHERE j.status IN('pending','retry')");
    if(rc!=FVS_OK)return rc;
    rc=fvs_i_exec(ctx,
        "UPDATE commerce_abandonment_cases ac "
        "JOIN orders o ON o.payment_attempt_id=ac.attempt_id AND o.status='confirmed' "
        "SET ac.state='converted',ac.converted_at=COALESCE(ac.converted_at,o.created_at),ac.updated_at=UTC_TIMESTAMP() "
        "WHERE ac.state<>'converted'");
    if(rc!=FVS_OK)return rc;

    char sql[8192];
    const int written=snprintf(sql,sizeof sql,
        "INSERT IGNORE INTO commerce_abandonment_cases("
        "attempt_id,cart_id,campaign_id,creative_id,channel,state,reason_code,detected_at,updated_at) "
        "SELECT p.id,p.cart_id,c.source_campaign_id,c.source_creative_id,p.source_channel,"
        "'detected','checkout_idle',UTC_TIMESTAMP(6),UTC_TIMESTAMP(6) "
        "FROM payment_attempts p JOIN carts c ON c.id=p.cart_id "
        "JOIN marketing_campaigns mc ON mc.id=c.source_campaign_id "
        "LEFT JOIN payment_recovery_cases pr ON pr.attempt_id=p.id "
        "LEFT JOIN orders o ON o.payment_attempt_id=p.id "
        "WHERE p.status IN('creating','pending') AND c.status='checkout' "
        "AND p.updated_at<=TIMESTAMPADD(SECOND,-%u,UTC_TIMESTAMP()) AND o.id IS NULL "
        "AND (pr.attempt_id IS NULL OR pr.state='active') "
        "AND mc.automation_mode='guarded' AND mc.status IN('approved','scheduled','active') "
        "AND p.source_channel IN('meta','instagram','linkedin','tiktok','x','google','email','webhook','pinterest','whatsapp','youtube') "
        "AND (c.source_creative_id IS NULL OR EXISTS("
        "SELECT 1 FROM marketing_creatives cr WHERE cr.id=c.source_creative_id "
        "AND cr.campaign_id=mc.id AND cr.status='approved')) "
        "ORDER BY p.updated_at,p.id LIMIT %u",idle_seconds,limit);
    if(written<0||(size_t)written>=sizeof sql)return FVS_ERR_INTERNAL;
    rc=fvs_i_exec(ctx,sql);
    if(rc!=FVS_OK)return rc;
    const unsigned long long newly_detected=mysql_affected_rows(ctx->db);

    MYSQL_RES *res=NULL;
    rc=fvs_i_queryf(ctx,&res,
        "SELECT attempt_id FROM commerce_abandonment_cases "
        "WHERE state='detected' AND job_id IS NULL ORDER BY detected_at,attempt_id LIMIT %u",limit);
    if(rc!=FVS_OK)return rc;
    char attempts[FVS_ABANDONMENT_MAX_SCAN][37];
    unsigned int count=0U;
    MYSQL_ROW row;
    while((row=mysql_fetch_row(res))!=NULL && count<limit){
        (void)snprintf(attempts[count],sizeof attempts[count],"%s",row[0]?row[0]:"");
        count++;
    }
    mysql_free_result(res);

    unsigned int scheduled=0U;
    for(unsigned int idx=0U;idx<count;idx++){
        char job_id[37];
        if(fvs_uuid_v4(job_id)!=FVS_OK)return FVS_ERR_INTERNAL;
        rc=fvs_i_exec(ctx,"START TRANSACTION");
        if(rc!=FVS_OK)return rc;
        char job_sql[8192];
        const int n=snprintf(job_sql,sizeof job_sql,
            "INSERT INTO marketing_jobs(id,campaign_id,creative_id,channel,action,payload_json,"
            "scheduled_at,status,attempts,next_attempt_at,created_at,updated_at) "
            "SELECT '%s',ac.campaign_id,ac.creative_id,ac.channel,'recover_abandonment',"
            "JSON_OBJECT('attempt_id',ac.attempt_id,'cart_id',ac.cart_id,'customer_email',p.email,"
            "'amount_minor',p.amount_minor,'currency',p.currency,'reason','checkout_idle'),"
            "UTC_TIMESTAMP(),'pending',0,UTC_TIMESTAMP(),UTC_TIMESTAMP(),UTC_TIMESTAMP() "
            "FROM commerce_abandonment_cases ac "
            "JOIN payment_attempts p ON p.id=ac.attempt_id "
            "JOIN marketing_campaigns mc ON mc.id=ac.campaign_id "
            "LEFT JOIN marketing_creatives cr ON cr.id=ac.creative_id "
            "WHERE ac.attempt_id='%s' AND ac.state='detected' AND ac.job_id IS NULL "
            "AND p.status IN('creating','pending') "
            "AND mc.automation_mode='guarded' AND mc.status IN('approved','scheduled','active') "
            "AND (ac.creative_id IS NULL OR (cr.campaign_id=mc.id AND cr.status='approved')) "
            "AND NOT EXISTS(SELECT 1 FROM orders o WHERE o.payment_attempt_id=ac.attempt_id)",
            job_id,attempts[idx]);
        if(n<0||(size_t)n>=sizeof job_sql){(void)fvs_i_exec(ctx,"ROLLBACK");return FVS_ERR_INTERNAL;}
        rc=fvs_i_exec(ctx,job_sql);
        if(rc!=FVS_OK){(void)fvs_i_exec(ctx,"ROLLBACK");return rc;}
        if(mysql_affected_rows(ctx->db)==1ULL){
            char case_sql[512];
            const int u=snprintf(case_sql,sizeof case_sql,
                "UPDATE commerce_abandonment_cases SET state='scheduled',job_id='%s',scheduled_at=UTC_TIMESTAMP(6),"
                "updated_at=UTC_TIMESTAMP(6) WHERE attempt_id='%s' AND state='detected' AND job_id IS NULL",
                job_id,attempts[idx]);
            if(u<0||(size_t)u>=sizeof case_sql){(void)fvs_i_exec(ctx,"ROLLBACK");return FVS_ERR_INTERNAL;}
            rc=fvs_i_exec(ctx,case_sql);
            if(rc!=FVS_OK||mysql_affected_rows(ctx->db)!=1ULL){
                (void)fvs_i_exec(ctx,"ROLLBACK");
                return rc==FVS_OK?FVS_ERR_CONFLICT:rc;
            }
            scheduled++;
        }
        rc=fvs_i_exec(ctx,"COMMIT");
        if(rc!=FVS_OK){(void)fvs_i_exec(ctx,"ROLLBACK");return rc;}
    }

    fvs_i_json w;
    fvs_i_json_init(&w,out,out_size);
    fvs_i_json_printf(&w,
        "{\"idle_seconds\":%u,\"newly_detected\":%llu,\"newly_scheduled\":%u}",
        idle_seconds,newly_detected,scheduled);
    return fvs_i_json_result(ctx,&w);
}

int fvs_abandonment_dashboard(fvs_ctx *ctx,unsigned int days,char *out,size_t out_size) {
    if(!ctx||days<1U||days>730U||!out)return FVS_ERR_INVALID;
    return abandonment_counts(ctx,days,out,out_size);
}
