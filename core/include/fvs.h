#ifndef FVS_H
#define FVS_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#  ifdef FVS_BUILD_DLL
#    define FVS_API __declspec(dllexport)
#  else
#    define FVS_API __declspec(dllimport)
#  endif
#else
#  define FVS_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fvs_ctx fvs_ctx;

typedef enum {
    FVS_OK = 0,
    FVS_ERR_INVALID = 1,
    FVS_ERR_DB = 2,
    FVS_ERR_NOT_FOUND = 3,
    FVS_ERR_CONFLICT = 4,
    FVS_ERR_AUTH = 5,
    FVS_ERR_EXPIRED = 6,
    FVS_ERR_STATE = 7,
    FVS_ERR_OVERFLOW = 8,
    FVS_ERR_BUFFER = 9,
    FVS_ERR_INTERNAL = 10,
    FVS_ERR_RETRY = 11
} fvs_status;

typedef enum {
    FVS_CLAIM_ACQUIRED = 1,
    FVS_CLAIM_BUSY = 2,
    FVS_CLAIM_DONE = 3
} fvs_claim_result;

FVS_API const char *fvs_version(void);
FVS_API const char *fvs_status_name(int status);
FVS_API const char *fvs_last_error(fvs_ctx *ctx);

FVS_API fvs_ctx *fvs_ctx_open(const char *host, unsigned int port,
                              const char *user, const char *password,
                              const char *database,
                              unsigned int connect_timeout_seconds);
FVS_API void fvs_ctx_close(fvs_ctx *ctx);
FVS_API int fvs_ping(fvs_ctx *ctx);

FVS_API int fvs_inventory_search(fvs_ctx *ctx,
                                 const char *check_in,
                                 const char *check_out,
                                 unsigned int guests,
                                 char *out_json, size_t out_size);
FVS_API int fvs_inventory_search_v2(fvs_ctx *ctx,
                                    const char *query, const char *check_in, const char *check_out,
                                    unsigned int guests, const char *country, const char *city,
                                    const char *resort, const char *property_type, const char *season,
                                    const char *booking_mode, const char *amenities_csv,
                                    unsigned int min_bedrooms, double min_bathrooms, unsigned int min_rating_x100,
                                    int64_t min_price_minor, int64_t max_price_minor,
                                    double min_lat, double min_lng, double max_lat, double max_lng,
                                    int use_bbox, const char *sort, unsigned int limit, unsigned int offset,
                                    char *out_json, size_t out_size);
FVS_API int fvs_inventory_suggest(fvs_ctx *ctx,const char *query,unsigned int limit,char *out_json,size_t out_size);
FVS_API int fvs_inventory_discovery_meta_upsert(fvs_ctx *ctx,const char *source_ref,int has_geo,double latitude,double longitude,const char *property_type,const char *amenities_json,unsigned int bedrooms,double bathrooms,unsigned int rating_x100);
FVS_API int fvs_inventory_upsert_v2(fvs_ctx *ctx,
                                    const char *source_ref,
                                    const char *resort,
                                    const char *unit_code,
                                    const char *unit_name,
                                    const char *city,
                                    const char *country,
                                    const char *check_in,
                                    const char *check_out,
                                    unsigned int week_number,
                                    const char *booking_mode,
                                    const char *float_group,
                                    unsigned int max_guests,
                                    int64_t price_minor,
                                    const char *currency,
                                    const char *image_url,
                                    const char *short_description,
                                    const char *season,
                                    int active,
                                    int *out_created);
FVS_API int fvs_inventory_upsert(fvs_ctx *ctx,
                                 const char *source_ref,
                                 const char *resort,
                                 const char *unit_code,
                                 const char *unit_name,
                                 const char *city,
                                 const char *country,
                                 const char *check_in,
                                 const char *check_out,
                                 unsigned int max_guests,
                                 int64_t price_minor,
                                 const char *currency,
                                 const char *image_url,
                                 const char *short_description,
                                 const char *season,
                                 int active,
                                 int *out_created);

FVS_API int fvs_cart_create(fvs_ctx *ctx,
                            char *out_cart_id, size_t cart_id_size,
                            char *out_secret, size_t secret_size);
FVS_API int fvs_cart_get(fvs_ctx *ctx,
                         const char *cart_id, const char *secret,
                         char *out_json, size_t out_size);
FVS_API int fvs_cart_add(fvs_ctx *ctx,
                         const char *cart_id, const char *secret,
                         const char *slot_id, unsigned int guests,
                         unsigned int hold_seconds,
                         char *out_json, size_t out_size);
FVS_API int fvs_cart_remove(fvs_ctx *ctx,
                            const char *cart_id, const char *secret,
                            const char *slot_id,
                            char *out_json, size_t out_size);

FVS_API int fvs_checkout_begin(fvs_ctx *ctx,
                               const char *cart_id, const char *secret,
                               const char *provider, const char *email,
                               const char *terms_version,
                               unsigned int hold_seconds,
                               char *out_json, size_t out_size);
FVS_API int fvs_checkout_attach_provider(fvs_ctx *ctx,
                                         const char *attempt_id,
                                         const char *provider_checkout_id,
                                         const char *provider_payment_id,
                                         const char *client_secret,
                                         const char *redirect_url);
FVS_API int fvs_checkout_get(fvs_ctx *ctx,
                             const char *cart_id, const char *secret,
                             char *out_json, size_t out_size);

FVS_API int fvs_payment_confirm(fvs_ctx *ctx,
                                const char *attempt_id,
                                const char *provider,
                                const char *provider_payment_id,
                                int64_t amount_minor,
                                const char *currency,
                                uint64_t checkout_version,
                                char *out_json, size_t out_size);

FVS_API int fvs_webhook_claim_v2(fvs_ctx *ctx,
                                 const char *provider,
                                 const char *event_id,
                                 const char *lease_owner,
                                 unsigned int lease_seconds,
                                 char *out_lease_token,
                                 size_t token_size,
                                 int *out_claim_result);
FVS_API int fvs_webhook_complete_v2(fvs_ctx *ctx,
                                    const char *provider,
                                    const char *event_id,
                                    const char *lease_owner,
                                    const char *lease_token,
                                    int success,
                                    const char *last_error);

/* Legacy owner-only webhook symbols remain binary-compatible in the library,
   but are intentionally omitted from the public API. Use the fenced v2 calls above. */
FVS_API int fvs_outbox_claim(fvs_ctx *ctx,
                             const char *lease_owner,
                             unsigned int lease_seconds,
                             char *out_json, size_t out_size);
FVS_API int fvs_outbox_ack(fvs_ctx *ctx,
                           const char *event_id,
                           const char *lease_owner,
                           const char *lease_token);
FVS_API int fvs_outbox_nack(fvs_ctx *ctx,
                            const char *event_id,
                            const char *lease_owner,
                            const char *lease_token,
                            const char *error_message);

FVS_API int fvs_order_get(fvs_ctx *ctx,
                          const char *order_id,
                          char *out_json, size_t out_size);

FVS_API int fvs_schema_status(fvs_ctx *ctx,
                              const char *required_migration,
                              char *out_json, size_t out_size);
FVS_API int fvs_ops_snapshot(fvs_ctx *ctx,
                             unsigned int stale_worker_seconds,
                             char *out_json, size_t out_size);
FVS_API int fvs_worker_heartbeat(fvs_ctx *ctx,
                                 const char *worker_name,
                                 const char *instance_id,
                                 const char *worker_version);
FVS_API int fvs_worker_goodbye(fvs_ctx *ctx,
                               const char *worker_name,
                               const char *instance_id);
FVS_API int fvs_outbox_requeue_dead(fvs_ctx *ctx,
                                    const char *event_id,
                                    const char *actor);
FVS_API int fvs_manual_review_list(fvs_ctx *ctx,
                                   unsigned int limit,
                                   char *out_json, size_t out_size);
FVS_API int fvs_ops_action_record(fvs_ctx *ctx,
                                  const char *actor,
                                  const char *action_type,
                                  const char *target_id);

FVS_API int fvs_maintenance_sweep(fvs_ctx *ctx,
                                  unsigned int open_cart_ttl_seconds,
                                  unsigned int *out_expired_carts,
                                  unsigned int *out_released_holds);


/* Hybrid AI + human support. Public customer calls authenticate with thread secret;
   agent/AI calls are trusted shim/admin operations. */
FVS_API int fvs_support_thread_create(fvs_ctx *ctx,
                                      const char *email,
                                      const char *subject,
                                      const char *cart_id,
                                      const char *order_id,
                                      const char *priority,
                                      char *out_thread_id, size_t thread_id_size,
                                      char *out_secret, size_t secret_size);
FVS_API int fvs_support_thread_get(fvs_ctx *ctx,
                                   const char *thread_id,
                                   const char *secret,
                                   char *out_json, size_t out_size);
FVS_API int fvs_support_thread_context(fvs_ctx *ctx,
                                       const char *thread_id,
                                       char *out_json, size_t out_size);
FVS_API int fvs_support_customer_message(fvs_ctx *ctx,
                                         const char *thread_id,
                                         const char *secret,
                                         const char *body,
                                         char *out_json, size_t out_size);
FVS_API int fvs_support_ai_message(fvs_ctx *ctx,
                                   const char *thread_id,
                                   const char *body,
                                   const char *model_name,
                                   unsigned int input_tokens,
                                   unsigned int output_tokens,
                                   char *out_json, size_t out_size);
FVS_API int fvs_support_handoff(fvs_ctx *ctx,
                                const char *thread_id,
                                const char *reason);
FVS_API int fvs_support_agent_heartbeat(fvs_ctx *ctx,
                                        const char *agent_id,
                                        const char *display_name,
                                        const char *status,
                                        unsigned int max_active);
FVS_API int fvs_support_queue_list(fvs_ctx *ctx,
                                   unsigned int limit,
                                   char *out_json, size_t out_size);
FVS_API int fvs_support_agent_claim(fvs_ctx *ctx,
                                    const char *thread_id,
                                    const char *agent_id);
FVS_API int fvs_support_agent_message(fvs_ctx *ctx,
                                      const char *thread_id,
                                      const char *agent_id,
                                      const char *body,
                                      int internal_only,
                                      char *out_json, size_t out_size);
FVS_API int fvs_support_thread_resolve(fvs_ctx *ctx,
                                       const char *thread_id,
                                       const char *agent_id);

/* Growth engine: approval-gated campaigns, creatives, jobs and attribution. */
FVS_API int fvs_marketing_campaign_create(fvs_ctx *ctx,
                                          const char *name,
                                          const char *objective,
                                          const char *automation_mode,
                                          int64_t budget_minor,
                                          int64_t daily_budget_minor,
                                          const char *currency,
                                          const char *utm_campaign,
                                          const char *actor,
                                          char *out_json, size_t out_size);
FVS_API int fvs_marketing_campaign_list(fvs_ctx *ctx,
                                        unsigned int limit,
                                        char *out_json, size_t out_size);
FVS_API int fvs_marketing_campaign_approve(fvs_ctx *ctx,
                                           const char *campaign_id,
                                           const char *actor);
FVS_API int fvs_marketing_campaign_configure(fvs_ctx *ctx,
                                             const char *campaign_id,
                                             int64_t max_daily_spend_minor,
                                             unsigned int frequency_cap_7d,
                                             unsigned int target_roas_bps,
                                             int64_t stop_loss_minor,
                                             const char *audience_json,
                                             const char *geo_json,
                                             const char *placements_json,
                                             const char *optimization_rules_json,
                                             const char *experiment_json,
                                             const char *actor);
FVS_API int fvs_marketing_campaign_pause(fvs_ctx *ctx,
                                         const char *campaign_id,
                                         const char *actor);
FVS_API int fvs_marketing_creative_add(fvs_ctx *ctx,
                                       const char *campaign_id,
                                       const char *channel,
                                       const char *variant_key,
                                       const char *headline,
                                       const char *body,
                                       const char *cta,
                                       const char *landing_url,
                                       const char *image_url,
                                       int ai_generated,
                                       const char *actor,
                                       char *out_json, size_t out_size);
FVS_API int fvs_marketing_creative_approve(fvs_ctx *ctx,
                                           const char *creative_id,
                                           const char *actor);
FVS_API int fvs_marketing_job_schedule(fvs_ctx *ctx,
                                       const char *campaign_id,
                                       const char *creative_id,
                                       const char *channel,
                                       const char *action,
                                       const char *scheduled_at,
                                       const char *payload_json,
                                       const char *actor,
                                       char *out_json, size_t out_size);
FVS_API int fvs_marketing_job_claim(fvs_ctx *ctx,
                                    const char *lease_owner,
                                    unsigned int lease_seconds,
                                    char *out_json, size_t out_size);
FVS_API int fvs_marketing_job_ack(fvs_ctx *ctx,
                                  const char *job_id,
                                  const char *lease_owner,
                                  const char *lease_token,
                                  const char *provider_ref);
FVS_API int fvs_marketing_job_nack(fvs_ctx *ctx,
                                   const char *job_id,
                                   const char *lease_owner,
                                   const char *lease_token,
                                   const char *error_message);
FVS_API int fvs_marketing_metric_upsert(fvs_ctx *ctx,
                                        const char *campaign_id,
                                        const char *channel,
                                        const char *metric_date,
                                        uint64_t impressions,
                                        uint64_t clicks,
                                        int64_t spend_minor,
                                        uint64_t leads,
                                        uint64_t bookings,
                                        int64_t revenue_minor,
                                        const char *currency);
FVS_API int fvs_marketing_attribution_record(fvs_ctx *ctx,
                                             const char *campaign_id,
                                             const char *channel,
                                             const char *creative_id,
                                             const char *visitor_id,
                                             const char *session_id,
                                             const char *order_id,
                                             const char *event_type,
                                             int64_t value_minor,
                                             const char *currency,
                                             const char *utm_source,
                                             const char *utm_medium,
                                             const char *utm_campaign,
                                             const char *utm_content,
                                             const char *referrer);
FVS_API int fvs_marketing_dashboard(fvs_ctx *ctx,
                                    unsigned int days,
                                    char *out_json, size_t out_size);




FVS_API int fvs_payment_recovery_mark(fvs_ctx *ctx,const char *attempt_id,const char *state,
                                      const char *reason_code,const char *last_error,
                                      unsigned int retry_after_seconds);
FVS_API int fvs_payment_recovery_get(fvs_ctx *ctx,const char *attempt_id,
                                     char *out_json,size_t out_size);
FVS_API int fvs_payment_recovery_dashboard(fvs_ctx *ctx,unsigned int days,
                                           char *out_json,size_t out_size);

/* Closed-loop growth: evidence-backed guarded pause/scale decisions. */
FVS_API int fvs_growth_loop_run(fvs_ctx *ctx,unsigned int days,unsigned int limit,
                                char *out_json,size_t out_size);
FVS_API int fvs_growth_decision_apply(fvs_ctx *ctx,const char *job_id);
FVS_API int fvs_growth_loop_dashboard(fvs_ctx *ctx,unsigned int days,
                                      char *out_json,size_t out_size);

/* Guarded checkout-abandonment recovery queue. */
FVS_API int fvs_abandonment_scan(fvs_ctx *ctx,unsigned int idle_seconds,unsigned int limit,
                                 char *out_json,size_t out_size);
FVS_API int fvs_abandonment_dashboard(fvs_ctx *ctx,unsigned int days,
                                      char *out_json,size_t out_size);

/* Evidence-only experiment science: no automatic winner selection. */
FVS_API int fvs_experiment_snapshot(fvs_ctx *ctx,const char *campaign_id,unsigned int days,
                                    char *out_json,size_t out_size);
FVS_API int fvs_experiment_dashboard(fvs_ctx *ctx,unsigned int days,
                                     char *out_json,size_t out_size);

/* Versioned search-science evidence and online quality telemetry. */
FVS_API const char *fvs_search_ranking_version(void);
FVS_API int fvs_search_eval_record(fvs_ctx *ctx,const char *ranking_version,
                                   const char *corpus_sha256,unsigned int mrr_ppm,
                                   unsigned int ndcg10_ppm,unsigned int precision10_ppm,
                                   unsigned int query_count,char *out_json,size_t out_size);
FVS_API int fvs_search_quality_dashboard(fvs_ctx *ctx,unsigned int days,
                                         char *out_json,size_t out_size);

/* Durable server-side commerce telemetry and funnel economics. */
FVS_API int fvs_commerce_event_record(fvs_ctx *ctx,
                                      const char *event_key,
                                      const char *event_type,
                                      const char *source,
                                      const char *visitor_id,
                                      const char *session_id,
                                      const char *cart_id,
                                      const char *attempt_id,
                                      const char *order_id,
                                      const char *slot_id,
                                      const char *campaign_id,
                                      const char *creative_id,
                                      const char *channel,
                                      const char *provider,
                                      const char *outcome,
                                      const char *reason_code,
                                      int64_t value_minor,
                                      const char *currency,
                                      const char *query_text,
                                      int result_count,
                                      const char *metadata_json);
FVS_API int fvs_commerce_funnel_dashboard(fvs_ctx *ctx,
                                          unsigned int days,
                                          char *out_json, size_t out_size);

/* Commerce Experience 6.x: server-authoritative merchandising and social commerce. */
FVS_API int fvs_commerce_home(fvs_ctx *ctx, char *out_json, size_t out_size);
FVS_API int fvs_commerce_collection_get(fvs_ctx *ctx, const char *slug, char *out_json, size_t out_size);
FVS_API int fvs_commerce_recommendations(fvs_ctx *ctx, const char *slot_id, unsigned int limit, char *out_json, size_t out_size);
FVS_API int fvs_cart_apply_code(fvs_ctx *ctx, const char *cart_id, const char *secret, const char *code, char *out_json, size_t out_size);
FVS_API int fvs_cart_addon_set(fvs_ctx *ctx, const char *cart_id, const char *secret, const char *addon_code, unsigned int quantity, char *out_json, size_t out_size);
FVS_API int fvs_cart_set_origin(fvs_ctx *ctx, const char *cart_id, const char *secret, const char *channel, const char *campaign_id, const char *creative_id, const char *social_token);
FVS_API int fvs_commerce_promotion_upsert(fvs_ctx *ctx, const char *code, const char *name, const char *discount_type, int64_t discount_value, int64_t min_subtotal_minor, int64_t max_discount_minor, const char *currency, const char *starts_at, const char *ends_at, uint64_t usage_limit, int active, const char *channel_scope_json, const char *actor);
FVS_API int fvs_commerce_collection_upsert(fvs_ctx *ctx, const char *slug, const char *name, const char *subtitle, const char *hero_image_url, const char *badge, const char *filter_json, const char *merchandising_json, int active, int sort_order, const char *actor);
FVS_API int fvs_social_catalog_feed(fvs_ctx *ctx, const char *channel, unsigned int limit, unsigned int offset, char *out_json, size_t out_size);
FVS_API int fvs_social_sales_link_create(fvs_ctx *ctx, const char *channel, const char *slot_id, const char *collection_slug, const char *campaign_id, const char *creative_id, const char *promo_code, const char *actor, char *out_json, size_t out_size);
FVS_API int fvs_social_sales_link_resolve(fvs_ctx *ctx, const char *token, char *out_json, size_t out_size);
FVS_API int fvs_social_lead_capture(fvs_ctx *ctx, const char *channel, const char *provider_lead_id, const char *contact_json, const char *message, const char *campaign_id, const char *creative_id, char *out_json, size_t out_size);

/* Programmatic SEO pages and redirects. Shims render HTML/XML server-side. */
FVS_API int fvs_seo_rebuild_inventory(fvs_ctx *ctx,
                                      const char *actor,
                                      unsigned int *out_upserted);
FVS_API int fvs_seo_page_upsert(fvs_ctx *ctx,
                                const char *inventory_slot_id,
                                const char *slug,
                                const char *locale,
                                const char *page_type,
                                const char *title,
                                const char *meta_description,
                                const char *h1,
                                const char *body_text,
                                const char *faq_json,
                                const char *canonical_path,
                                const char *og_image_url,
                                int indexable,
                                const char *actor);
FVS_API int fvs_seo_page_get(fvs_ctx *ctx,
                             const char *slug,
                             const char *locale,
                             char *out_json, size_t out_size);
FVS_API int fvs_seo_pages_list(fvs_ctx *ctx,
                               unsigned int limit,
                               char *out_json, size_t out_size);
FVS_API int fvs_seo_redirect_get(fvs_ctx *ctx,
                                 const char *source_path,
                                 char *out_json, size_t out_size);

/* Pure helpers: exported for tests and language shims. */
FVS_API int fvs_uuid_v4(char out[37]);
FVS_API int fvs_random_hex(char *out, size_t out_size, size_t random_bytes);
FVS_API int fvs_sha256_hex(const char *text, char out[65]);
FVS_API int fvs_secure_equals(const char *a, const char *b);
FVS_API int fvs_currency_valid(const char *currency);
FVS_API int fvs_email_valid(const char *email);
FVS_API int fvs_provider_valid(const char *provider);
FVS_API int fvs_add_i64_checked(int64_t a, int64_t b, int64_t *out);
FVS_API int fvs_json_escape(const char *input, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
#endif
