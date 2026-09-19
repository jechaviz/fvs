<?php declare(strict_types=1);
final class CoreException extends RuntimeException { public function __construct(public int $coreCode,string $message){parent::__construct($message,$coreCode);} }
final class FvsCore {
    private FFI $ffi; private ?FFI\CData $ctx=null; private const BUF=4194304;
    public function __construct(bool $connect=true){
        $lib=getenv('FVS_CORE_LIB') ?: dirname(__DIR__).'/core/build/release/libfvs_core.so';
        try {
            $this->ffi=FFI::scope('FVS');
        } catch (Throwable $e) {
            if((getenv('FVS_ENV')?:'development')==='production') throw new RuntimeException('FVS FFI preload scope unavailable',0,$e);
            // Development/test fallback. Production PHP-FPM must preload the ABI once at process start.
            $this->ffi=FFI::cdef(file_get_contents(__DIR__.'/fvs_ffi.h'),$lib);
        }
        if($connect){
            $ctx=$this->ffi->fvs_ctx_open(getenv('FVS_DB_HOST')?:'127.0.0.1',(int)(getenv('FVS_DB_PORT')?:3306),getenv('FVS_DB_USER')?:'fvs',getenv('FVS_DB_PASSWORD')?:'',getenv('FVS_DB_NAME')?:'fvs',(int)(getenv('FVS_DB_CONNECT_TIMEOUT')?:5));
            if(FFI::isNull($ctx)) throw new CoreException(2,'database connection failed');
            $this->ctx=$ctx;
        }
    }
    public function __destruct(){ if($this->ctx!==null && !FFI::isNull($this->ctx)) $this->ffi->fvs_ctx_close($this->ctx); }
    private static function cstr(mixed $v):string{return is_string($v)?$v:FFI::string($v);}
    private function check(int $rc):void { if($rc!==0) throw new CoreException($rc,self::cstr($this->ffi->fvs_last_error($this->ctx))); }
    private function jsonCall(string $fn,array $args=[]):array { $buf=FFI::new('char['.self::BUF.']');$args=array_merge([$this->ctx],$args,[$buf,self::BUF]);$rc=$this->ffi->$fn(...$args);$this->check($rc);$v=json_decode(FFI::string($buf),true,512,JSON_THROW_ON_ERROR);return is_array($v)?$v:[]; }
    public function version():string{return self::cstr($this->ffi->fvs_version());}
    public function ping():void{$this->check($this->ffi->fvs_ping($this->ctx));}
    public function inventory(string $ci,string $co,int $g):array{return $this->jsonCall('fvs_inventory_search',[$ci,$co,$g]);}
    public function inventorySearch(array $f):array{$bbox=$f['bbox']??null;$use=is_array($bbox)&&count($bbox)===4?1:0;if(!$use)$bbox=[0,0,0,0];return $this->jsonCall('fvs_inventory_search_v2',[(string)($f['q']??''),(string)($f['check_in']??''),(string)($f['check_out']??''),(int)($f['guests']??2),(string)($f['country']??''),(string)($f['city']??''),(string)($f['resort']??''),(string)($f['property_type']??''),(string)($f['season']??''),(string)($f['booking_mode']??''),(string)($f['amenities']??''),(int)($f['min_bedrooms']??0),(float)($f['min_bathrooms']??0),(int)($f['min_rating_x100']??0),(int)($f['min_price_minor']??0),(int)($f['max_price_minor']??0),(float)$bbox[0],(float)$bbox[1],(float)$bbox[2],(float)$bbox[3],$use,(string)($f['sort']??'relevance'),(int)($f['limit']??60),(int)($f['offset']??0)]);}
    public function inventorySuggest(string $q,int $limit=10):array{return $this->jsonCall('fvs_inventory_suggest',[$q,$limit]);}
    public function inventoryUpsert(array $r):bool{$created=FFI::new('int');$this->check($this->ffi->fvs_inventory_upsert_v2($this->ctx,$r['source_ref'],$r['resort'],$r['unit_code'],$r['unit_name'],$r['city']??'',$r['country']??'',$r['check_in'],$r['check_out'],(int)($r['week_number']??0),$r['booking_mode']??'fixed',$r['float_group']??'',(int)$r['max_guests'],(int)$r['price_minor'],$r['currency'],$r['image_url']??'',$r['short_description']??'',$r['season']??'',!empty($r['active'])?1:0,FFI::addr($created)));$am=$r['amenities']??[];if(is_string($am))$am=array_values(array_filter(array_map('trim',preg_split('/[,;]/',$am)?:[])));$lat=$r['latitude']??null;$lng=$r['longitude']??null;$has=($lat!==null&&$lat!==''&&$lng!==null&&$lng!=='')?1:0;$json=json_encode($am,JSON_UNESCAPED_UNICODE|JSON_UNESCAPED_SLASHES|JSON_THROW_ON_ERROR);$this->check($this->ffi->fvs_inventory_discovery_meta_upsert($this->ctx,$r['source_ref'],$has,(float)($lat?:0),(float)($lng?:0),(string)($r['property_type']??''),$json,(int)($r['bedrooms']??0),(float)($r['bathrooms']??0),(int)round(((float)($r['rating']??0))*100)));return (int)$created->cdata===1;}
    public function cartCreate():array{$id=FFI::new('char[37]');$sec=FFI::new('char[65]');$this->check($this->ffi->fvs_cart_create($this->ctx,$id,37,$sec,65));return [FFI::string($id),FFI::string($sec)];}
    public function cartGet(string $id,string $secret):array{return $this->jsonCall('fvs_cart_get',[$id,$secret]);}
    public function cartAdd(string $id,string $secret,string $slot,int $guests,int $hold):array{return $this->jsonCall('fvs_cart_add',[$id,$secret,$slot,$guests,$hold]);}
    public function cartRemove(string $id,string $secret,string $slot):array{return $this->jsonCall('fvs_cart_remove',[$id,$secret,$slot]);}
    public function checkoutBegin(string $id,string $secret,string $provider,string $email,string $terms,int $hold):array{return $this->jsonCall('fvs_checkout_begin',[$id,$secret,$provider,$email,$terms,$hold]);}
    public function checkoutGet(string $id,string $secret):array{return $this->jsonCall('fvs_checkout_get',[$id,$secret]);}
    public function checkoutAttach(string $attempt,?string $checkout,?string $payment,?string $clientSecret,?string $redirect):void{$this->check($this->ffi->fvs_checkout_attach_provider($this->ctx,$attempt,$checkout,$payment,$clientSecret,$redirect));}
    public function paymentConfirm(string $attempt,string $provider,string $payment,int $amount,string $currency,int $version):array{return $this->jsonCall('fvs_payment_confirm',[$attempt,$provider,$payment,$amount,$currency,$version]);}
    public function webhookClaim(string $provider,string $event,string $owner,int $ttl=120):array{$out=FFI::new('int');$token=FFI::new('char[33]');$this->check($this->ffi->fvs_webhook_claim_v2($this->ctx,$provider,$event,$owner,$ttl,$token,33,FFI::addr($out)));return [(int)$out->cdata,FFI::string($token)];}
    public function webhookComplete(string $provider,string $event,string $owner,string $token,bool $success,?string $error=null):void{$this->check($this->ffi->fvs_webhook_complete_v2($this->ctx,$provider,$event,$owner,$token,$success?1:0,$error));}
    public function outboxClaim(string $owner,int $ttl=120):array{return $this->jsonCall('fvs_outbox_claim',[$owner,$ttl]);}
    public function outboxAck(string $event,string $owner,string $token):void{$this->check($this->ffi->fvs_outbox_ack($this->ctx,$event,$owner,$token));}
    public function outboxNack(string $event,string $owner,string $token,string $error):void{$this->check($this->ffi->fvs_outbox_nack($this->ctx,$event,$owner,$token,$error));}
    public function orderGet(string $id):array{return $this->jsonCall('fvs_order_get',[$id]);}
    public function maintenanceSweep(int $openCartTtl=86400):array{$expired=FFI::new('unsigned int');$released=FFI::new('unsigned int');$this->check($this->ffi->fvs_maintenance_sweep($this->ctx,$openCartTtl,FFI::addr($expired),FFI::addr($released)));return ['expired_carts'=>(int)$expired->cdata,'released_holds'=>(int)$released->cdata];}

    public function schemaStatus(string $required='001_current.sql'):array{return $this->jsonCall('fvs_schema_status',[$required]);}
    public function opsSnapshot(int $staleWorkerSeconds=90):array{return $this->jsonCall('fvs_ops_snapshot',[$staleWorkerSeconds]);}
    public function workerHeartbeat(string $workerName,string $instanceId,?string $workerVersion=null):void{$this->check($this->ffi->fvs_worker_heartbeat($this->ctx,$workerName,$instanceId,$workerVersion??$this->version()));}
    public function workerGoodbye(string $workerName,string $instanceId):void{$this->check($this->ffi->fvs_worker_goodbye($this->ctx,$workerName,$instanceId));}
    public function outboxRequeueDead(string $eventId,string $actor):void{$this->check($this->ffi->fvs_outbox_requeue_dead($this->ctx,$eventId,$actor));}
    public function manualReviewList(int $limit=100):array{return $this->jsonCall('fvs_manual_review_list',[$limit]);}
    public function opsActionRecord(string $actor,string $actionType,string $targetId):void{$this->check($this->ffi->fvs_ops_action_record($this->ctx,$actor,$actionType,$targetId));}

    public function supportThreadCreate(string $email,string $subject,?string $cartId=null,?string $orderId=null,string $priority='normal'):array{$id=FFI::new('char[37]');$sec=FFI::new('char[65]');$this->check($this->ffi->fvs_support_thread_create($this->ctx,$email,$subject,$cartId??'',$orderId??'',$priority,$id,37,$sec,65));return [FFI::string($id),FFI::string($sec)];}
    public function supportThreadGet(string $threadId,string $secret):array{return $this->jsonCall('fvs_support_thread_get',[$threadId,$secret]);}
    public function supportThreadContext(string $threadId):array{return $this->jsonCall('fvs_support_thread_context',[$threadId]);}
    public function supportCustomerMessage(string $threadId,string $secret,string $body):array{return $this->jsonCall('fvs_support_customer_message',[$threadId,$secret,$body]);}
    public function supportAiMessage(string $threadId,string $body,string $model='',int $inputTokens=0,int $outputTokens=0):array{return $this->jsonCall('fvs_support_ai_message',[$threadId,$body,$model,$inputTokens,$outputTokens]);}
    public function supportHandoff(string $threadId,string $reason):void{$this->check($this->ffi->fvs_support_handoff($this->ctx,$threadId,$reason));}
    public function supportAgentHeartbeat(string $agentId,string $name,string $status='available',int $maxActive=5):void{$this->check($this->ffi->fvs_support_agent_heartbeat($this->ctx,$agentId,$name,$status,$maxActive));}
    public function supportQueueList(int $limit=100):array{return $this->jsonCall('fvs_support_queue_list',[$limit]);}
    public function supportAgentClaim(string $threadId,string $agentId):void{$this->check($this->ffi->fvs_support_agent_claim($this->ctx,$threadId,$agentId));}
    public function supportAgentMessage(string $threadId,string $agentId,string $body,bool $internal=false):array{return $this->jsonCall('fvs_support_agent_message',[$threadId,$agentId,$body,$internal?1:0]);}
    public function supportThreadResolve(string $threadId,string $agentId):void{$this->check($this->ffi->fvs_support_thread_resolve($this->ctx,$threadId,$agentId));}

    public function marketingCampaignCreate(string $name,string $objective,string $mode,int $budget,int $daily,string $currency,string $utm,string $actor):array{return $this->jsonCall('fvs_marketing_campaign_create',[$name,$objective,$mode,$budget,$daily,$currency,$utm,$actor]);}
    public function marketingCampaignList(int $limit=100):array{return $this->jsonCall('fvs_marketing_campaign_list',[$limit]);}
    public function marketingCampaignApprove(string $id,string $actor):void{$this->check($this->ffi->fvs_marketing_campaign_approve($this->ctx,$id,$actor));}
    public function marketingCampaignConfigure(string $id,array $v,string $actor):void{$json=static fn($x)=>json_encode($x??[],JSON_UNESCAPED_UNICODE|JSON_UNESCAPED_SLASHES);$this->check($this->ffi->fvs_marketing_campaign_configure($this->ctx,$id,(int)($v['max_daily_spend_minor']??0),(int)($v['frequency_cap_7d']??6),(int)($v['target_roas_bps']??0),(int)($v['stop_loss_minor']??0),$json($v['audience']??[]),$json($v['geo']??[]),$json($v['placements']??[]),$json($v['optimization_rules']??[]),$json($v['experiment']??[]),$actor));}
    public function marketingCampaignPause(string $id,string $actor):void{$this->check($this->ffi->fvs_marketing_campaign_pause($this->ctx,$id,$actor));}
    public function marketingCreativeAdd(string $campaign,string $channel,string $variant,string $headline,string $body,string $cta,string $landing,string $image,bool $ai,string $actor):array{return $this->jsonCall('fvs_marketing_creative_add',[$campaign,$channel,$variant,$headline,$body,$cta,$landing,$image,$ai?1:0,$actor]);}
    public function marketingCreativeApprove(string $id,string $actor):void{$this->check($this->ffi->fvs_marketing_creative_approve($this->ctx,$id,$actor));}
    public function marketingJobSchedule(string $campaign,string $creative,string $channel,string $action,string $scheduled,array $payload,string $actor):array{return $this->jsonCall('fvs_marketing_job_schedule',[$campaign,$creative,$channel,$action,$scheduled,json_encode($payload,JSON_UNESCAPED_UNICODE|JSON_UNESCAPED_SLASHES),$actor]);}
    public function marketingJobClaim(string $owner,int $ttl=120):array{return $this->jsonCall('fvs_marketing_job_claim',[$owner,$ttl]);}
    public function marketingJobAck(string $job,string $owner,string $token,string $providerRef=''):void{$this->check($this->ffi->fvs_marketing_job_ack($this->ctx,$job,$owner,$token,$providerRef));}
    public function marketingJobNack(string $job,string $owner,string $token,string $error):void{$this->check($this->ffi->fvs_marketing_job_nack($this->ctx,$job,$owner,$token,$error));}
    public function marketingMetricUpsert(string $campaign,string $channel,string $date,int $impressions,int $clicks,int $spend,int $leads,int $bookings,int $revenue,string $currency):void{$this->check($this->ffi->fvs_marketing_metric_upsert($this->ctx,$campaign,$channel,$date,$impressions,$clicks,$spend,$leads,$bookings,$revenue,$currency));}
    public function marketingAttributionRecord(array $v):void{$this->check($this->ffi->fvs_marketing_attribution_record($this->ctx,(string)($v['campaign_id']??''),(string)($v['channel']??''),(string)($v['creative_id']??''),(string)($v['visitor_id']??''),(string)($v['session_id']??''),(string)($v['order_id']??''),(string)($v['event_type']??'landing'),(int)($v['value_minor']??0),(string)($v['currency']??''),(string)($v['utm_source']??''),(string)($v['utm_medium']??''),(string)($v['utm_campaign']??''),(string)($v['utm_content']??''),(string)($v['referrer']??'')));}
    public function marketingDashboard(int $days=30):array{return $this->jsonCall('fvs_marketing_dashboard',[$days]);}

    public function commerceHome():array{return $this->jsonCall('fvs_commerce_home',[]);}
    public function commerceCollectionGet(string $slug):array{return $this->jsonCall('fvs_commerce_collection_get',[$slug]);}
    public function commerceRecommendations(string $slotId,int $limit=8):array{return $this->jsonCall('fvs_commerce_recommendations',[$slotId,$limit]);}
    public function cartApplyCode(string $cartId,string $secret,string $code):array{return $this->jsonCall('fvs_cart_apply_code',[$cartId,$secret,$code]);}
    public function cartAddonSet(string $cartId,string $secret,string $code,int $quantity):array{return $this->jsonCall('fvs_cart_addon_set',[$cartId,$secret,$code,$quantity]);}
    public function cartSetOrigin(string $cartId,string $secret,string $channel='',string $campaignId='',string $creativeId='',string $socialToken=''):void{$this->check($this->ffi->fvs_cart_set_origin($this->ctx,$cartId,$secret,$channel,$campaignId,$creativeId,$socialToken));}
    public function commercePromotionUpsert(array $v,string $actor='commerce-admin'):void{$this->check($this->ffi->fvs_commerce_promotion_upsert($this->ctx,$v['code'],$v['name'],$v['discount_type'],(int)$v['discount_value'],(int)($v['min_subtotal_minor']??0),(int)($v['max_discount_minor']??0),$v['currency']??'MXN',$v['starts_at']??'',$v['ends_at']??'',(int)($v['usage_limit']??0),!empty($v['active'])?1:0,json_encode($v['channel_scope']??[],JSON_UNESCAPED_SLASHES),$actor));}
    public function commerceCollectionUpsert(array $v,string $actor='commerce-admin'):void{$json=static fn($x)=>json_encode($x??[],JSON_UNESCAPED_UNICODE|JSON_UNESCAPED_SLASHES);$this->check($this->ffi->fvs_commerce_collection_upsert($this->ctx,$v['slug'],$v['name'],$v['subtitle']??'',$v['hero_image_url']??'',$v['badge']??'',$json($v['filter']??[]),$json($v['merchandising']??[]),!empty($v['active'])?1:0,(int)($v['sort_order']??0),$actor));}
    public function socialCatalogFeed(string $channel,int $limit=500,int $offset=0):array{return $this->jsonCall('fvs_social_catalog_feed',[$channel,$limit,$offset]);}
    public function socialSalesLinkCreate(array $v,string $actor='social-admin'):array{return $this->jsonCall('fvs_social_sales_link_create',[$v['channel'],$v['slot_id']??'',$v['collection_slug']??'',$v['campaign_id']??'',$v['creative_id']??'',$v['promo_code']??'',$actor]);}
    public function socialSalesLinkResolve(string $token):array{return $this->jsonCall('fvs_social_sales_link_resolve',[$token]);}
    public function socialLeadCapture(array $v):array{return $this->jsonCall('fvs_social_lead_capture',[$v['channel'],$v['provider_lead_id'],json_encode($v['contact']??[],JSON_UNESCAPED_UNICODE|JSON_UNESCAPED_SLASHES),$v['message'],$v['campaign_id']??'',$v['creative_id']??'']);}

    public function seoRebuildInventory(string $actor='seo-admin'):int{$n=FFI::new('unsigned int');$this->check($this->ffi->fvs_seo_rebuild_inventory($this->ctx,$actor,FFI::addr($n)));return (int)$n->cdata;}
    public function seoPageUpsert(array $v,string $actor='seo-admin'):void{$faq=json_encode($v['faq']??[],JSON_UNESCAPED_UNICODE|JSON_UNESCAPED_SLASHES);$this->check($this->ffi->fvs_seo_page_upsert($this->ctx,$v['inventory_slot_id']??'',$v['slug'],$v['locale']??'es-MX',$v['page_type']??'property',$v['title'],$v['meta_description'],$v['h1'],$v['body_text']??'',$faq,$v['canonical_path'],$v['og_image_url']??'',!empty($v['indexable'])?1:0,$actor));}
    public function seoPageGet(string $slug,string $locale='es-MX'):array{return $this->jsonCall('fvs_seo_page_get',[$slug,$locale]);}
    public function seoPagesList(int $limit=50000):array{return $this->jsonCall('fvs_seo_pages_list',[$limit]);}
    public function seoRedirectGet(string $path):array{return $this->jsonCall('fvs_seo_redirect_get',[$path]);}

}
