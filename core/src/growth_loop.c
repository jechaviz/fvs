#include "internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FVS_GROWTH_MAX_CAMPAIGNS 100U
#define FVS_GROWTH_MAX_CHANNELS 16U

typedef struct {
    char campaign_id[37];
    unsigned long long budget_minor;
    unsigned long long daily_budget_minor;
    unsigned long long max_daily_spend_minor;
    unsigned int target_roas_bps;
    unsigned long long stop_loss_minor;
    char currency[4];
    unsigned long long min_spend_minor;
    unsigned long long min_bookings;
    unsigned int pause_below_roas_bps;
    unsigned int scale_up_bps;
    unsigned long long total_spend_minor;
} growth_campaign;

static unsigned long long u64(const char *text) {
    return text && *text ? strtoull(text,NULL,10) : 0ULL;
}

static unsigned int u32(const char *text) {
    const unsigned long long value=u64(text);
    return value>4294967295ULL?4294967295U:(unsigned int)value;
}

static unsigned int roas_bps(unsigned long long revenue,unsigned long long spend) {
    if(spend==0ULL)return 0U;
    const long double ratio=((long double)revenue*10000.0L)/(long double)spend;
    return ratio>4294967295.0L?4294967295U:(unsigned int)(ratio+0.5L);
}

static int make_decision(fvs_ctx *ctx,const growth_campaign *c,const char *channel,
                         const char *day,const char *action,const char *reason,
                         unsigned long long spend,unsigned long long provider_revenue,
                         unsigned long long authoritative_revenue,unsigned long long bookings,
                         unsigned int authoritative_roas,unsigned int provider_roas,
                         unsigned long long proposed,unsigned int window_days,
                         unsigned int *scheduled) {
    char key[191];
    const int keyn=snprintf(key,sizeof key,"%s:%s:%s:%s",c->campaign_id,channel,day,action);
    if(keyn<0||(size_t)keyn>=sizeof key)return FVS_ERR_INTERNAL;

    MYSQL_RES *res=NULL;
    int rc=fvs_i_queryf(ctx,&res,"SELECT id FROM marketing_growth_decisions WHERE decision_key='%s'",key);
    if(rc!=FVS_OK)return rc;
    MYSQL_ROW row=mysql_fetch_row(res);
    if(row){mysql_free_result(res);return FVS_OK;}
    mysql_free_result(res);

    char decision_id[37],job_id[37];
    if(fvs_uuid_v4(decision_id)!=FVS_OK||fvs_uuid_v4(job_id)!=FVS_OK)return FVS_ERR_INTERNAL;

    const char *job_action=!strcmp(action,"increase")?"update_budget":"pause";
    char payload[2048];
    const int pn=snprintf(payload,sizeof payload,
        "{\"source\":\"closed_loop_growth\",\"decision_id\":\"%s\",\"reason\":\"%s\","
        "\"window_days\":%u,\"spend_minor\":%llu,\"provider_revenue_minor\":%llu,"
        "\"authoritative_revenue_minor\":%llu,\"bookings\":%llu,"
        "\"authoritative_roas_bps\":%u,\"provider_roas_bps\":%u,"
        "\"previous_daily_budget_minor\":%llu,\"new_daily_budget_minor\":%llu}",
        decision_id,reason,window_days,spend,provider_revenue,authoritative_revenue,bookings,
        authoritative_roas,provider_roas,c->daily_budget_minor,proposed);
    if(pn<0||(size_t)pn>=sizeof payload)return FVS_ERR_INTERNAL;

    rc=fvs_i_exec(ctx,"START TRANSACTION");
    if(rc!=FVS_OK)return rc;

    char sql[8192];
    int n=snprintf(sql,sizeof sql,
        "INSERT INTO marketing_jobs(id,campaign_id,creative_id,channel,action,payload_json,scheduled_at,status,attempts,next_attempt_at,created_at,updated_at) "
        "VALUES('%s','%s',NULL,'%s','%s',CAST('%s' AS JSON),UTC_TIMESTAMP(6),'pending',0,UTC_TIMESTAMP(6),UTC_TIMESTAMP(6),UTC_TIMESTAMP(6))",
        job_id,c->campaign_id,channel,job_action,payload);
    if(n<0||(size_t)n>=sizeof sql){(void)fvs_i_exec(ctx,"ROLLBACK");return FVS_ERR_INTERNAL;}
    rc=fvs_i_exec(ctx,sql);
    if(rc!=FVS_OK){(void)fvs_i_exec(ctx,"ROLLBACK");return rc;}

    n=snprintf(sql,sizeof sql,
        "INSERT INTO marketing_growth_decisions(id,decision_key,campaign_id,channel,action,reason_code,window_days,"
        "spend_minor,provider_revenue_minor,authoritative_revenue_minor,bookings,authoritative_roas_bps,provider_roas_bps,"
        "previous_daily_budget_minor,proposed_daily_budget_minor,job_id,state,created_at,updated_at) "
        "VALUES('%s','%s','%s','%s','%s','%s',%u,%llu,%llu,%llu,%llu,%u,%u,%llu,%llu,'%s','scheduled',UTC_TIMESTAMP(6),UTC_TIMESTAMP(6))",
        decision_id,key,c->campaign_id,channel,action,reason,window_days,spend,provider_revenue,
        authoritative_revenue,bookings,authoritative_roas,provider_roas,c->daily_budget_minor,proposed,job_id);
    if(n<0||(size_t)n>=sizeof sql){(void)fvs_i_exec(ctx,"ROLLBACK");return FVS_ERR_INTERNAL;}
    rc=fvs_i_exec(ctx,sql);
    if(rc!=FVS_OK){(void)fvs_i_exec(ctx,"ROLLBACK");return rc;}

    rc=fvs_i_exec(ctx,"COMMIT");
    if(rc!=FVS_OK){(void)fvs_i_exec(ctx,"ROLLBACK");return rc;}
    (*scheduled)++;
    return FVS_OK;
}

int fvs_growth_loop_run(fvs_ctx *ctx,unsigned int days,unsigned int limit,
                        char *out,size_t out_size) {
    if(!ctx||days<1U||days>90U||limit<1U||limit>FVS_GROWTH_MAX_CAMPAIGNS||!out)return FVS_ERR_INVALID;

    MYSQL_RES *res=NULL;
    int rc=fvs_i_queryf(ctx,&res,
        "SELECT c.id,c.budget_minor,c.daily_budget_minor,c.max_daily_spend_minor,c.target_roas_bps,c.stop_loss_minor,c.currency,"
        "COALESCE(CAST(JSON_UNQUOTE(JSON_EXTRACT(c.optimization_rules_json,'$.min_spend_minor')) AS UNSIGNED),0),"
        "COALESCE(CAST(JSON_UNQUOTE(JSON_EXTRACT(c.optimization_rules_json,'$.min_bookings')) AS UNSIGNED),0),"
        "COALESCE(CAST(JSON_UNQUOTE(JSON_EXTRACT(c.optimization_rules_json,'$.pause_below_roas_bps')) AS UNSIGNED),0),"
        "COALESCE(CAST(JSON_UNQUOTE(JSON_EXTRACT(c.optimization_rules_json,'$.scale_up_bps')) AS UNSIGNED),0),"
        "COALESCE((SELECT SUM(md.spend_minor) FROM marketing_metrics_daily md WHERE md.campaign_id=c.id),0) "
        "FROM marketing_campaigns c WHERE c.automation_mode='guarded' "
        "AND c.status IN('approved','scheduled','active') AND c.target_roas_bps>0 "
        "ORDER BY c.updated_at,c.id LIMIT %u",limit);
    if(rc!=FVS_OK)return rc;

    growth_campaign campaigns[FVS_GROWTH_MAX_CAMPAIGNS];
    unsigned int campaign_count=0U;
    MYSQL_ROW row;
    while((row=mysql_fetch_row(res))!=NULL&&campaign_count<limit){
        growth_campaign *c=&campaigns[campaign_count++];
        (void)snprintf(c->campaign_id,sizeof c->campaign_id,"%s",row[0]?row[0]:"");
        c->budget_minor=u64(row[1]);c->daily_budget_minor=u64(row[2]);
        c->max_daily_spend_minor=u64(row[3]);c->target_roas_bps=u32(row[4]);
        c->stop_loss_minor=u64(row[5]);(void)snprintf(c->currency,sizeof c->currency,"%s",row[6]?row[6]:"");
        c->min_spend_minor=u64(row[7]);c->min_bookings=u64(row[8]);
        c->pause_below_roas_bps=u32(row[9]);c->scale_up_bps=u32(row[10]);c->total_spend_minor=u64(row[11]);
    }
    mysql_free_result(res);

    unsigned int scheduled=0U,considered=0U,suppressed=0U;
    for(unsigned int ci=0U;ci<campaign_count;ci++){
        growth_campaign *c=&campaigns[ci];
        if(c->min_spend_minor==0ULL||(c->scale_up_bps>10000U)||(c->pause_below_roas_bps>1000000U)){
            suppressed++;continue;
        }

        rc=fvs_i_queryf(ctx,&res,
            "SELECT channel,COALESCE(SUM(spend_minor),0),COALESCE(SUM(revenue_minor),0),"
            "COALESCE(SUM(bookings),0),COUNT(DISTINCT currency),"
            "COALESCE(SUM(currency<>'%s'),0),DATE_FORMAT(UTC_DATE(),'%%Y-%%m-%%d') "
            "FROM marketing_metrics_daily WHERE campaign_id='%s' "
            "AND metric_date>=DATE(TIMESTAMPADD(DAY,-%u,UTC_TIMESTAMP())) "
            "GROUP BY channel ORDER BY channel LIMIT %u",
            c->currency,c->campaign_id,days,FVS_GROWTH_MAX_CHANNELS);
        if(rc!=FVS_OK)return rc;

        struct channel_row {char channel[16];unsigned long long spend,provider_revenue,provider_bookings;char day[11];} channels[FVS_GROWTH_MAX_CHANNELS];
        unsigned int channel_count=0U;
        while((row=mysql_fetch_row(res))!=NULL&&channel_count<FVS_GROWTH_MAX_CHANNELS){
            if(u64(row[4])!=1ULL||u64(row[5])!=0ULL){suppressed++;continue;}
            struct channel_row *m=&channels[channel_count++];
            (void)snprintf(m->channel,sizeof m->channel,"%s",row[0]?row[0]:"");
            m->spend=u64(row[1]);m->provider_revenue=u64(row[2]);m->provider_bookings=u64(row[3]);
            (void)snprintf(m->day,sizeof m->day,"%s",row[6]?row[6]:"");
        }
        mysql_free_result(res);

        for(unsigned int mi=0U;mi<channel_count;mi++){
            struct channel_row *m=&channels[mi];
            considered++;
            if(m->spend<c->min_spend_minor){suppressed++;continue;}

            rc=fvs_i_queryf(ctx,&res,
                "SELECT COUNT(*),COALESCE(SUM(total_minor),0) FROM orders "
                "WHERE source_campaign_id='%s' AND source_channel='%s' AND status='confirmed' "
                "AND currency='%s' AND created_at>=TIMESTAMPADD(DAY,-%u,UTC_TIMESTAMP())",
                c->campaign_id,m->channel,c->currency,days);
            if(rc!=FVS_OK)return rc;
            row=mysql_fetch_row(res);
            const unsigned long long bookings=row&&row[0]?u64(row[0]):0ULL;
            const unsigned long long revenue=row&&row[1]?u64(row[1]):0ULL;
            mysql_free_result(res);
            if(bookings<c->min_bookings){suppressed++;continue;}

            const unsigned int auth_roas=roas_bps(revenue,m->spend);
            const unsigned int provider_roas=roas_bps(m->provider_revenue,m->spend);
            const unsigned long long loss=m->spend>revenue?m->spend-revenue:0ULL;

            if((c->stop_loss_minor>0ULL&&loss>=c->stop_loss_minor)||
               (c->pause_below_roas_bps>0U&&auth_roas<=c->pause_below_roas_bps)){
                const char *reason=(c->stop_loss_minor>0ULL&&loss>=c->stop_loss_minor)?"stop_loss":"roas_below_floor";
                rc=make_decision(ctx,c,m->channel,m->day,"pause",reason,m->spend,m->provider_revenue,
                                 revenue,bookings,auth_roas,provider_roas,c->daily_budget_minor,days,&scheduled);
                if(rc!=FVS_OK)return rc;
                continue;
            }

            if(c->scale_up_bps>0U&&c->daily_budget_minor>0ULL&&c->max_daily_spend_minor>c->daily_budget_minor&&
               auth_roas>=c->target_roas_bps&&provider_roas>=c->target_roas_bps){
                const unsigned long long increment=
                    (c->daily_budget_minor/10000ULL)*(unsigned long long)c->scale_up_bps+
                    ((c->daily_budget_minor%10000ULL)*(unsigned long long)c->scale_up_bps)/10000ULL;
                unsigned long long proposed=increment>ULLONG_MAX-c->daily_budget_minor?
                    ULLONG_MAX:c->daily_budget_minor+increment;
                if(proposed>c->max_daily_spend_minor)proposed=c->max_daily_spend_minor;
                if(c->budget_minor>0ULL){
                    const unsigned long long remaining=c->budget_minor>c->total_spend_minor?
                        c->budget_minor-c->total_spend_minor:0ULL;
                    if(proposed>remaining)proposed=remaining;
                }
                if(proposed>c->daily_budget_minor){
                    rc=make_decision(ctx,c,m->channel,m->day,"increase","roas_above_target",m->spend,
                                     m->provider_revenue,revenue,bookings,auth_roas,provider_roas,proposed,days,&scheduled);
                    if(rc!=FVS_OK)return rc;
                } else suppressed++;
            } else suppressed++;
        }
    }

    fvs_i_json w;fvs_i_json_init(&w,out,out_size);
    fvs_i_json_printf(&w,
        "{\"days\":%u,\"campaigns\":%u,\"channels_considered\":%u,\"scheduled\":%u,\"suppressed\":%u}",
        days,campaign_count,considered,scheduled,suppressed);
    return fvs_i_json_result(ctx,&w);
}

int fvs_growth_decision_apply(fvs_ctx *ctx,const char *job_id) {
    if(!ctx||!fvs_i_valid_uuid(job_id))return FVS_ERR_INVALID;
    int rc=fvs_i_exec(ctx,"START TRANSACTION");
    if(rc!=FVS_OK)return rc;

    MYSQL_RES *res=NULL;
    rc=fvs_i_queryf(ctx,&res,
        "SELECT d.id,d.campaign_id,d.action,d.reason_code,d.previous_daily_budget_minor,d.proposed_daily_budget_minor,"
        "d.state,c.daily_budget_minor,c.status FROM marketing_growth_decisions d "
        "JOIN marketing_campaigns c ON c.id=d.campaign_id WHERE d.job_id='%s' FOR UPDATE",job_id);
    if(rc!=FVS_OK){(void)fvs_i_exec(ctx,"ROLLBACK");return rc;}
    MYSQL_ROW row=mysql_fetch_row(res);
    if(!row){mysql_free_result(res);return fvs_i_exec(ctx,"COMMIT");}

    char decision_id[37],campaign_id[37],action[16],reason[81],state[16],status[24];
    (void)snprintf(decision_id,sizeof decision_id,"%s",row[0]?row[0]:"");
    (void)snprintf(campaign_id,sizeof campaign_id,"%s",row[1]?row[1]:"");
    (void)snprintf(action,sizeof action,"%s",row[2]?row[2]:"");
    (void)snprintf(reason,sizeof reason,"%s",row[3]?row[3]:"");
    const unsigned long long previous=u64(row[4]),proposed=u64(row[5]),current=u64(row[7]);
    (void)snprintf(state,sizeof state,"%s",row[6]?row[6]:"");
    (void)snprintf(status,sizeof status,"%s",row[8]?row[8]:"");
    mysql_free_result(res);

    if(!strcmp(state,"applied")||!strcmp(state,"superseded"))return fvs_i_exec(ctx,"COMMIT");

    char sql[4096];
    int n;
    const char *budget_action=NULL;
    if(!strcmp(action,"increase")){
        if(current!=previous||!strcmp(status,"paused")||!strcmp(status,"completed")||!strcmp(status,"cancelled")){
            n=snprintf(sql,sizeof sql,"UPDATE marketing_growth_decisions SET state='superseded',updated_at=UTC_TIMESTAMP(6) WHERE id='%s'",decision_id);
            if(n<0||(size_t)n>=sizeof sql){(void)fvs_i_exec(ctx,"ROLLBACK");return FVS_ERR_INTERNAL;}
            rc=fvs_i_exec(ctx,sql);
            if(rc!=FVS_OK){(void)fvs_i_exec(ctx,"ROLLBACK");return rc;}
            return fvs_i_exec(ctx,"COMMIT");
        }
        n=snprintf(sql,sizeof sql,
            "UPDATE marketing_campaigns SET daily_budget_minor=%llu,updated_at=UTC_TIMESTAMP(6) WHERE id='%s' AND daily_budget_minor=%llu",
            proposed,campaign_id,previous);
        budget_action="increase";
    } else if(!strcmp(action,"pause")){
        if(!strcmp(status,"completed")||!strcmp(status,"cancelled")){
            n=snprintf(sql,sizeof sql,"UPDATE marketing_growth_decisions SET state='superseded',updated_at=UTC_TIMESTAMP(6) WHERE id='%s'",decision_id);
            if(n<0||(size_t)n>=sizeof sql){(void)fvs_i_exec(ctx,"ROLLBACK");return FVS_ERR_INTERNAL;}
            rc=fvs_i_exec(ctx,sql);
            if(rc!=FVS_OK){(void)fvs_i_exec(ctx,"ROLLBACK");return rc;}
            return fvs_i_exec(ctx,"COMMIT");
        }
        n=snprintf(sql,sizeof sql,
            "UPDATE marketing_campaigns SET status='paused',updated_at=UTC_TIMESTAMP(6) WHERE id='%s'",campaign_id);
        budget_action=!strcmp(reason,"stop_loss")?"stop_loss":"pause";
    } else {
        (void)fvs_i_exec(ctx,"ROLLBACK");return FVS_ERR_STATE;
    }
    if(n<0||(size_t)n>=sizeof sql){(void)fvs_i_exec(ctx,"ROLLBACK");return FVS_ERR_INTERNAL;}
    rc=fvs_i_exec(ctx,sql);
    if(rc!=FVS_OK){(void)fvs_i_exec(ctx,"ROLLBACK");return rc;}

    char event_id[37];
    if(fvs_uuid_v4(event_id)!=FVS_OK){(void)fvs_i_exec(ctx,"ROLLBACK");return FVS_ERR_INTERNAL;}
    n=snprintf(sql,sizeof sql,
        "INSERT INTO marketing_budget_events(id,campaign_id,actor,action,previous_daily_budget_minor,new_daily_budget_minor,detail_json,created_at) "
        "VALUES('%s','%s','closed-loop-growth','%s',%llu,%llu,JSON_OBJECT('decision_id','%s','reason','%s','job_id','%s'),UTC_TIMESTAMP(6))",
        event_id,campaign_id,budget_action,previous,!strcmp(action,"increase")?proposed:current,decision_id,reason,job_id);
    if(n<0||(size_t)n>=sizeof sql){(void)fvs_i_exec(ctx,"ROLLBACK");return FVS_ERR_INTERNAL;}
    rc=fvs_i_exec(ctx,sql);
    if(rc!=FVS_OK){(void)fvs_i_exec(ctx,"ROLLBACK");return rc;}

    n=snprintf(sql,sizeof sql,
        "UPDATE marketing_growth_decisions SET state='applied',applied_at=UTC_TIMESTAMP(6),updated_at=UTC_TIMESTAMP(6) WHERE id='%s'",
        decision_id);
    if(n<0||(size_t)n>=sizeof sql){(void)fvs_i_exec(ctx,"ROLLBACK");return FVS_ERR_INTERNAL;}
    rc=fvs_i_exec(ctx,sql);
    if(rc!=FVS_OK){(void)fvs_i_exec(ctx,"ROLLBACK");return rc;}

    rc=fvs_i_exec(ctx,"COMMIT");
    if(rc!=FVS_OK){(void)fvs_i_exec(ctx,"ROLLBACK");return rc;}
    return FVS_OK;
}

int fvs_growth_loop_dashboard(fvs_ctx *ctx,unsigned int days,char *out,size_t out_size) {
    if(!ctx||days<1U||days>730U||!out)return FVS_ERR_INVALID;
    MYSQL_RES *res=NULL;
    int rc=fvs_i_queryf(ctx,&res,
        "SELECT COUNT(*),COALESCE(SUM(state='scheduled'),0),COALESCE(SUM(state='applied'),0),"
        "COALESCE(SUM(state='superseded'),0),COALESCE(SUM(action='pause'),0),COALESCE(SUM(action='increase'),0) "
        "FROM marketing_growth_decisions WHERE created_at>=TIMESTAMPADD(DAY,-%u,UTC_TIMESTAMP())",days);
    if(rc!=FVS_OK)return rc;
    MYSQL_ROW row=mysql_fetch_row(res);
    const unsigned long long total=row&&row[0]?u64(row[0]):0ULL,scheduled=row&&row[1]?u64(row[1]):0ULL,
        applied=row&&row[2]?u64(row[2]):0ULL,superseded=row&&row[3]?u64(row[3]):0ULL,
        pauses=row&&row[4]?u64(row[4]):0ULL,increases=row&&row[5]?u64(row[5]):0ULL;
    mysql_free_result(res);

    fvs_i_json w;fvs_i_json_init(&w,out,out_size);
    fvs_i_json_printf(&w,
        "{\"days\":%u,\"total\":%llu,\"scheduled\":%llu,\"applied\":%llu,\"superseded\":%llu,"
        "\"pause\":%llu,\"increase\":%llu,\"recent\":[",
        days,total,scheduled,applied,superseded,pauses,increases);

    rc=fvs_i_queryf(ctx,&res,
        "SELECT decision_key,campaign_id,channel,action,reason_code,spend_minor,authoritative_revenue_minor,"
        "bookings,authoritative_roas_bps,provider_roas_bps,previous_daily_budget_minor,proposed_daily_budget_minor,state,"
        "DATE_FORMAT(created_at,'%%Y-%%m-%%dT%%H:%%i:%%sZ') FROM marketing_growth_decisions "
        "WHERE created_at>=TIMESTAMPADD(DAY,-%u,UTC_TIMESTAMP()) ORDER BY created_at DESC,id DESC LIMIT 50",days);
    if(rc!=FVS_OK)return rc;
    int first=1;
    while((row=mysql_fetch_row(res))!=NULL){
        if(!first){fvs_i_json_puts(&w,",");}
        first=0;
        fvs_i_json_puts(&w,"{\"decision_key\":");fvs_i_json_string(&w,row[0]);
        fvs_i_json_puts(&w,",\"campaign_id\":");fvs_i_json_string(&w,row[1]);
        fvs_i_json_puts(&w,",\"channel\":");fvs_i_json_string(&w,row[2]);
        fvs_i_json_puts(&w,",\"action\":");fvs_i_json_string(&w,row[3]);
        fvs_i_json_puts(&w,",\"reason\":");fvs_i_json_string(&w,row[4]);
        fvs_i_json_printf(&w,
            ",\"spend_minor\":%llu,\"authoritative_revenue_minor\":%llu,\"bookings\":%llu,"
            "\"authoritative_roas_bps\":%u,\"provider_roas_bps\":%u,"
            "\"previous_daily_budget_minor\":%llu,\"proposed_daily_budget_minor\":%llu,\"state\":",
            u64(row[5]),u64(row[6]),u64(row[7]),u32(row[8]),u32(row[9]),u64(row[10]),u64(row[11]));
        fvs_i_json_string(&w,row[12]);fvs_i_json_puts(&w,",\"created_at\":");fvs_i_json_string(&w,row[13]);fvs_i_json_puts(&w,"}");
    }
    mysql_free_result(res);
    fvs_i_json_puts(&w,"]}");
    return fvs_i_json_result(ctx,&w);
}
