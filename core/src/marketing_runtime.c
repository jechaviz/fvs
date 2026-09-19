#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int text_ok(const char *text,size_t max_len) {
    return text&&*text&&strlen(text)<=max_len;
}

int fvs_marketing_job_claim(fvs_ctx *ctx,const char *owner,unsigned int lease_seconds,
                            char *out,size_t out_size) {
    if(!ctx||!text_ok(owner,128U)||lease_seconds<5U||lease_seconds>3600U||!out)return FVS_ERR_INVALID;
    char *eo=fvs_i_escape(ctx,owner);
    if(!eo)return FVS_ERR_INTERNAL;
    char token[33];
    if(fvs_random_hex(token,sizeof token,16U)!=FVS_OK){free(eo);return FVS_ERR_INTERNAL;}

    int rc=fvs_i_exec(ctx,"START TRANSACTION");
    if(rc!=FVS_OK){free(eo);return rc;}

    MYSQL_RES *res=NULL;
    rc=fvs_i_queryf(ctx,&res,
        "SELECT j.id,j.campaign_id,COALESCE(j.creative_id,''),j.channel,j.action,j.payload_json,j.attempts,"
        "COALESCE(c.name,''),COALESCE(mc.headline,''),COALESCE(mc.body,''),COALESCE(mc.cta,''),"
        "COALESCE(mc.landing_url,''),COALESCE(mc.image_url,''),c.max_daily_spend_minor,c.frequency_cap_7d,"
        "c.target_roas_bps,c.stop_loss_minor "
        "FROM marketing_jobs j JOIN marketing_campaigns c ON c.id=j.campaign_id "
        "LEFT JOIN marketing_creatives mc ON mc.id=j.creative_id "
        "WHERE j.attempts<8 AND j.scheduled_at<=UTC_TIMESTAMP() "
        "AND ((j.status IN('pending','retry') AND j.next_attempt_at<=UTC_TIMESTAMP()) OR (j.status='processing' AND j.lease_until<=UTC_TIMESTAMP())) "
        "AND (j.lease_until IS NULL OR j.lease_until<=UTC_TIMESTAMP()) "
        "AND (j.action IN('pause','sync_metrics','send_conversion') "
        "OR (j.action='update_budget' "
        "AND JSON_UNQUOTE(JSON_EXTRACT(j.payload_json,'$.source'))='closed_loop_growth' "
        "AND c.status IN('approved','scheduled','active') "
        "AND (c.target_roas_bps=0 "
        "OR COALESCE((SELECT SUM(gs.spend_minor) FROM marketing_metrics_daily gs WHERE gs.campaign_id=c.id),0)=0 "
        "OR (CAST(COALESCE((SELECT SUM(gr.revenue_minor) FROM marketing_metrics_daily gr WHERE gr.campaign_id=c.id),0) AS DECIMAL(30,6))/"
        "NULLIF(CAST(COALESCE((SELECT SUM(gx.spend_minor) FROM marketing_metrics_daily gx WHERE gx.campaign_id=c.id),0) AS DECIMAL(30,6)),0)*10000)>=c.target_roas_bps)) "
        "OR (c.status IN('approved','scheduled','active') "
        "AND (c.daily_budget_minor=0 OR COALESCE((SELECT SUM(md.spend_minor) FROM marketing_metrics_daily md WHERE md.campaign_id=c.id AND md.metric_date=UTC_DATE()),0)<c.daily_budget_minor) "
        "AND (c.max_daily_spend_minor=0 OR COALESCE((SELECT SUM(mx.spend_minor) FROM marketing_metrics_daily mx WHERE mx.campaign_id=c.id AND mx.metric_date=UTC_DATE()),0)<c.max_daily_spend_minor) "
        "AND (c.budget_minor=0 OR COALESCE((SELECT SUM(ma.spend_minor) FROM marketing_metrics_daily ma WHERE ma.campaign_id=c.id),0)<c.budget_minor) "
        "AND (c.stop_loss_minor=0 OR GREATEST(COALESCE((SELECT SUM(ms.spend_minor) FROM marketing_metrics_daily ms WHERE ms.campaign_id=c.id),0)-COALESCE((SELECT SUM(mr.revenue_minor) FROM marketing_metrics_daily mr WHERE mr.campaign_id=c.id),0),0)<c.stop_loss_minor) "
        "AND (j.action<>'update_budget' OR c.target_roas_bps=0 OR COALESCE((SELECT SUM(mt.spend_minor) FROM marketing_metrics_daily mt WHERE mt.campaign_id=c.id),0)=0 "
        "OR ((CAST(COALESCE((SELECT SUM(mv.revenue_minor) FROM marketing_metrics_daily mv WHERE mv.campaign_id=c.id),0) AS DECIMAL(30,6))/NULLIF(CAST(COALESCE((SELECT SUM(mz.spend_minor) FROM marketing_metrics_daily mz WHERE mz.campaign_id=c.id),0) AS DECIMAL(30,6)),0))*10000)>=c.target_roas_bps))) "
        "ORDER BY j.scheduled_at,j.id LIMIT 1 FOR UPDATE SKIP LOCKED");
    if(rc!=FVS_OK){(void)fvs_i_exec(ctx,"ROLLBACK");free(eo);return rc;}

    MYSQL_ROW row=mysql_fetch_row(res);
    if(!row){
        mysql_free_result(res);
        rc=fvs_i_exec(ctx,"COMMIT");
        free(eo);
        if(rc!=FVS_OK)return rc;
        fvs_i_json w;fvs_i_json_init(&w,out,out_size);fvs_i_json_puts(&w,"{}");return fvs_i_json_result(ctx,&w);
    }

    char id[37],campaign[37],creative[37],channel[16],action[24],payload[12001],name[181],
         headline[256],body[12001],cta[81],landing[1501],image[1501],
         maxdaily[32],freqcap[16],targetroas[16],stoploss[32];
    (void)snprintf(id,sizeof id,"%s",row[0]?row[0]:"");
    (void)snprintf(campaign,sizeof campaign,"%s",row[1]?row[1]:"");
    (void)snprintf(creative,sizeof creative,"%s",row[2]?row[2]:"");
    (void)snprintf(channel,sizeof channel,"%s",row[3]?row[3]:"");
    (void)snprintf(action,sizeof action,"%s",row[4]?row[4]:"");
    (void)snprintf(payload,sizeof payload,"%s",row[5]?row[5]:"{}");
    const unsigned int attempts=row[6]?(unsigned int)strtoul(row[6],NULL,10):0U;
    (void)snprintf(name,sizeof name,"%s",row[7]?row[7]:"");
    (void)snprintf(headline,sizeof headline,"%s",row[8]?row[8]:"");
    (void)snprintf(body,sizeof body,"%s",row[9]?row[9]:"");
    (void)snprintf(cta,sizeof cta,"%s",row[10]?row[10]:"");
    (void)snprintf(landing,sizeof landing,"%s",row[11]?row[11]:"");
    (void)snprintf(image,sizeof image,"%s",row[12]?row[12]:"");
    (void)snprintf(maxdaily,sizeof maxdaily,"%s",row[13]?row[13]:"0");
    (void)snprintf(freqcap,sizeof freqcap,"%s",row[14]?row[14]:"0");
    (void)snprintf(targetroas,sizeof targetroas,"%s",row[15]?row[15]:"0");
    (void)snprintf(stoploss,sizeof stoploss,"%s",row[16]?row[16]:"0");
    mysql_free_result(res);

    char sql[1024];
    const int n=snprintf(sql,sizeof sql,
        "UPDATE marketing_jobs SET status='processing',attempts=attempts+1,lease_owner='%s',lease_token='%s',"
        "lease_until=TIMESTAMPADD(SECOND,%u,UTC_TIMESTAMP()),updated_at=UTC_TIMESTAMP() WHERE id='%s'",
        eo,token,lease_seconds,id);
    if(n<0||(size_t)n>=sizeof sql){(void)fvs_i_exec(ctx,"ROLLBACK");free(eo);return FVS_ERR_INTERNAL;}
    rc=fvs_i_exec(ctx,sql);
    free(eo);
    if(rc!=FVS_OK||mysql_affected_rows(ctx->db)!=1ULL){
        (void)fvs_i_exec(ctx,"ROLLBACK");
        return rc==FVS_OK?FVS_ERR_CONFLICT:rc;
    }
    rc=fvs_i_exec(ctx,"COMMIT");
    if(rc!=FVS_OK){(void)fvs_i_exec(ctx,"ROLLBACK");return rc;}

    fvs_i_json w;fvs_i_json_init(&w,out,out_size);
    fvs_i_json_puts(&w,"{\"job_id\":");fvs_i_json_string(&w,id);
    fvs_i_json_puts(&w,",\"campaign_id\":");fvs_i_json_string(&w,campaign);
    fvs_i_json_puts(&w,",\"creative_id\":");creative[0]?fvs_i_json_string(&w,creative):fvs_i_json_puts(&w,"null");
    fvs_i_json_puts(&w,",\"channel\":");fvs_i_json_string(&w,channel);
    fvs_i_json_puts(&w,",\"action\":");fvs_i_json_string(&w,action);
    fvs_i_json_puts(&w,",\"lease_token\":");fvs_i_json_string(&w,token);
    fvs_i_json_printf(&w,",\"attempts\":%u,\"campaign_name\":",attempts+1U);fvs_i_json_string(&w,name);
    fvs_i_json_puts(&w,",\"headline\":");fvs_i_json_string(&w,headline);
    fvs_i_json_puts(&w,",\"body\":");fvs_i_json_string(&w,body);
    fvs_i_json_puts(&w,",\"cta\":");fvs_i_json_string(&w,cta);
    fvs_i_json_puts(&w,",\"landing_url\":");fvs_i_json_string(&w,landing);
    fvs_i_json_puts(&w,",\"image_url\":");fvs_i_json_string(&w,image);
    fvs_i_json_puts(&w,",\"payload\":");fvs_i_json_puts(&w,payload);
    fvs_i_json_printf(&w,
        ",\"guardrails\":{\"max_daily_spend_minor\":%s,\"frequency_cap_7d\":%s,"
        "\"target_roas_bps\":%s,\"stop_loss_minor\":%s}}",
        maxdaily,freqcap,targetroas,stoploss);
    return fvs_i_json_result(ctx,&w);
}
