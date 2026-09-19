#include "internal.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FVS_EXPERIMENT_MAX_VARIANTS 100U

typedef struct {
    char creative_id[37];
    char variant_key[65];
    unsigned long long exposures;
    unsigned long long bookings;
    unsigned int rate_ppm;
    unsigned int low_ppm;
    unsigned int high_ppm;
    int consistent;
    int sufficient;
} experiment_row;

static unsigned int ppm(double value) {
    if (value <= 0.0) return 0U;
    if (value >= 1.0) return 1000000U;
    return (unsigned int)(value * 1000000.0 + 0.5);
}

static void wilson95(unsigned long long successes, unsigned long long trials,
                     unsigned int *out_rate, unsigned int *out_low,
                     unsigned int *out_high, int *out_consistent) {
    *out_rate=0U;*out_low=0U;*out_high=0U;*out_consistent=0;
    if (trials==0ULL || successes>trials) return;
    const double n=(double)trials;
    const double p=(double)successes/n;
    const double z=1.959963984540054;
    const double z2=z*z;
    const double denom=1.0+z2/n;
    const double center=(p+z2/(2.0*n))/denom;
    const double margin=(z/denom)*sqrt((p*(1.0-p)+z2/(4.0*n))/n);
    *out_rate=ppm(p);
    *out_low=ppm(center-margin);
    *out_high=ppm(center+margin);
    *out_consistent=1;
}

int fvs_experiment_snapshot(fvs_ctx *ctx,const char *campaign_id,unsigned int days,
                            char *out,size_t out_size) {
    if(!ctx||!fvs_i_valid_uuid(campaign_id)||days<1U||days>730U||!out)return FVS_ERR_INVALID;

    MYSQL_RES *res=NULL;
    int rc=fvs_i_queryf(ctx,&res,"SELECT id FROM marketing_campaigns WHERE id='%s'",campaign_id);
    if(rc!=FVS_OK)return rc;
    MYSQL_ROW row=mysql_fetch_row(res);
    if(!row){mysql_free_result(res);return FVS_ERR_NOT_FOUND;}
    mysql_free_result(res);

    rc=fvs_i_queryf(ctx,&res,
        "SELECT mc.id,mc.variant_key,"
        "COALESCE((SELECT COUNT(DISTINCT COALESCE(ma.session_id,ma.visitor_id)) "
        "FROM marketing_attribution_events ma WHERE ma.campaign_id=mc.campaign_id AND ma.creative_id=mc.id "
        "AND ma.event_type='landing' AND ma.created_at>=TIMESTAMPADD(DAY,-%u,UTC_TIMESTAMP())),0),"
        "COALESCE((SELECT COUNT(*) FROM orders o WHERE o.source_campaign_id=mc.campaign_id "
        "AND o.source_creative_id=mc.id AND o.status='confirmed' "
        "AND o.created_at>=TIMESTAMPADD(DAY,-%u,UTC_TIMESTAMP())),0) "
        "FROM marketing_creatives mc WHERE mc.campaign_id='%s' AND mc.status='approved' "
        "ORDER BY mc.variant_key,mc.id LIMIT %u",days,days,campaign_id,FVS_EXPERIMENT_MAX_VARIANTS);
    if(rc!=FVS_OK)return rc;

    experiment_row rows[FVS_EXPERIMENT_MAX_VARIANTS];
    unsigned int row_count=0U;
    while((row=mysql_fetch_row(res))!=NULL && row_count<FVS_EXPERIMENT_MAX_VARIANTS){
        experiment_row *item=&rows[row_count++];
        (void)snprintf(item->creative_id,sizeof item->creative_id,"%s",row[0]?row[0]:"");
        (void)snprintf(item->variant_key,sizeof item->variant_key,"%s",row[1]?row[1]:"");
        item->exposures=row[2]?strtoull(row[2],NULL,10):0ULL;
        item->bookings=row[3]?strtoull(row[3],NULL,10):0ULL;
        wilson95(item->bookings,item->exposures,&item->rate_ppm,&item->low_ppm,&item->high_ppm,&item->consistent);
        item->sufficient=item->consistent && item->exposures>=30ULL;
    }
    mysql_free_result(res);

    char snapshot_id[37];
    if(fvs_uuid_v4(snapshot_id)!=FVS_OK)return FVS_ERR_INTERNAL;
    rc=fvs_i_exec(ctx,"START TRANSACTION");
    if(rc!=FVS_OK)return rc;

    for(unsigned int idx=0U;idx<row_count;idx++){
        experiment_row *item=&rows[idx];
        char evidence_id[37];
        if(fvs_uuid_v4(evidence_id)!=FVS_OK){(void)fvs_i_exec(ctx,"ROLLBACK");return FVS_ERR_INTERNAL;}
        char *ev=fvs_i_escape(ctx,item->variant_key);
        if(!ev){(void)fvs_i_exec(ctx,"ROLLBACK");return FVS_ERR_INTERNAL;}
        const int needed=snprintf(NULL,0,
            "INSERT INTO marketing_experiment_evidence(id,snapshot_id,campaign_id,creative_id,variant_key,window_days,exposures,bookings,conversion_ppm,wilson_low_ppm,wilson_high_ppm,attribution_consistent,sufficient_sample,created_at) "
            "VALUES('%s','%s','%s','%s','%s',%u,%llu,%llu,%u,%u,%u,%d,%d,UTC_TIMESTAMP(6))",
            evidence_id,snapshot_id,campaign_id,item->creative_id,ev,days,item->exposures,item->bookings,
            item->rate_ppm,item->low_ppm,item->high_ppm,item->consistent,item->sufficient);
        if(needed<0){free(ev);(void)fvs_i_exec(ctx,"ROLLBACK");return FVS_ERR_INTERNAL;}
        char *sql=malloc((size_t)needed+1U);
        if(!sql){free(ev);(void)fvs_i_exec(ctx,"ROLLBACK");return FVS_ERR_INTERNAL;}
        (void)snprintf(sql,(size_t)needed+1U,
            "INSERT INTO marketing_experiment_evidence(id,snapshot_id,campaign_id,creative_id,variant_key,window_days,exposures,bookings,conversion_ppm,wilson_low_ppm,wilson_high_ppm,attribution_consistent,sufficient_sample,created_at) "
            "VALUES('%s','%s','%s','%s','%s',%u,%llu,%llu,%u,%u,%u,%d,%d,UTC_TIMESTAMP(6))",
            evidence_id,snapshot_id,campaign_id,item->creative_id,ev,days,item->exposures,item->bookings,
            item->rate_ppm,item->low_ppm,item->high_ppm,item->consistent,item->sufficient);
        free(ev);
        rc=fvs_i_exec(ctx,sql);
        free(sql);
        if(rc!=FVS_OK){(void)fvs_i_exec(ctx,"ROLLBACK");return rc;}
    }
    rc=fvs_i_exec(ctx,"COMMIT");
    if(rc!=FVS_OK){(void)fvs_i_exec(ctx,"ROLLBACK");return rc;}

    fvs_i_json w;
    fvs_i_json_init(&w,out,out_size);
    fvs_i_json_puts(&w,"{\"snapshot_id\":");
    fvs_i_json_string(&w,snapshot_id);
    fvs_i_json_puts(&w,",\"campaign_id\":");
    fvs_i_json_string(&w,campaign_id);
    fvs_i_json_printf(&w,",\"window_days\":%u,\"automatic_winner_selection\":false,\"variants\":[",days);
    for(unsigned int idx=0U;idx<row_count;idx++){
        experiment_row *item=&rows[idx];
        if(idx>0U)fvs_i_json_puts(&w,",");
        fvs_i_json_puts(&w,"{\"creative_id\":");fvs_i_json_string(&w,item->creative_id);
        fvs_i_json_puts(&w,",\"variant_key\":");fvs_i_json_string(&w,item->variant_key);
        fvs_i_json_printf(&w,
            ",\"exposures\":%llu,\"bookings\":%llu,\"conversion\":%.6f,\"wilson95\":[%.6f,%.6f],"
            "\"attribution_consistent\":%s,\"sufficient_sample\":%s}",
            item->exposures,item->bookings,(double)item->rate_ppm/1000000.0,
            (double)item->low_ppm/1000000.0,(double)item->high_ppm/1000000.0,
            item->consistent?"true":"false",item->sufficient?"true":"false");
    }
    fvs_i_json_printf(&w,"],\"variant_count\":%u}",row_count);
    return fvs_i_json_result(ctx,&w);
}

int fvs_experiment_dashboard(fvs_ctx *ctx,unsigned int days,char *out,size_t out_size) {
    if(!ctx||days<1U||days>730U||!out)return FVS_ERR_INVALID;
    MYSQL_RES *res=NULL;
    const int rc=fvs_i_queryf(ctx,&res,
        "SELECT e.snapshot_id,e.campaign_id,c.name,e.creative_id,e.variant_key,e.exposures,e.bookings,"
        "e.conversion_ppm,e.wilson_low_ppm,e.wilson_high_ppm,e.attribution_consistent,e.sufficient_sample,"
        "DATE_FORMAT(e.created_at,'%%Y-%%m-%%dT%%H:%%i:%%s.%%fZ') "
        "FROM marketing_experiment_evidence e JOIN marketing_campaigns c ON c.id=e.campaign_id "
        "WHERE e.created_at>=TIMESTAMPADD(DAY,-%u,UTC_TIMESTAMP()) "
        "ORDER BY e.created_at DESC,e.snapshot_id,e.variant_key LIMIT 200",days);
    if(rc!=FVS_OK)return rc;

    fvs_i_json w;
    fvs_i_json_init(&w,out,out_size);
    fvs_i_json_printf(&w,"{\"days\":%u,\"automatic_winner_selection\":false,\"evidence\":[",days);
    int first=1;
    MYSQL_ROW row;
    while((row=mysql_fetch_row(res))!=NULL){
        if(!first)fvs_i_json_puts(&w,",");
        first=0;
        fvs_i_json_puts(&w,"{\"snapshot_id\":");fvs_i_json_string(&w,row[0]);
        fvs_i_json_puts(&w,",\"campaign_id\":");fvs_i_json_string(&w,row[1]);
        fvs_i_json_puts(&w,",\"campaign_name\":");fvs_i_json_string(&w,row[2]);
        fvs_i_json_puts(&w,",\"creative_id\":");fvs_i_json_string(&w,row[3]);
        fvs_i_json_puts(&w,",\"variant_key\":");fvs_i_json_string(&w,row[4]);
        fvs_i_json_printf(&w,
            ",\"exposures\":%llu,\"bookings\":%llu,\"conversion\":%.6f,\"wilson95\":[%.6f,%.6f],"
            "\"attribution_consistent\":%s,\"sufficient_sample\":%s,\"created_at\":",
            row[5]?strtoull(row[5],NULL,10):0ULL,row[6]?strtoull(row[6],NULL,10):0ULL,
            row[7]?(double)strtoul(row[7],NULL,10)/1000000.0:0.0,
            row[8]?(double)strtoul(row[8],NULL,10)/1000000.0:0.0,
            row[9]?(double)strtoul(row[9],NULL,10)/1000000.0:0.0,
            row[10]&&atoi(row[10])?"true":"false",row[11]&&atoi(row[11])?"true":"false");
        fvs_i_json_string(&w,row[12]?row[12]:"");
        fvs_i_json_puts(&w,"}");
    }
    mysql_free_result(res);
    fvs_i_json_puts(&w,"]}");
    return fvs_i_json_result(ctx,&w);
}
