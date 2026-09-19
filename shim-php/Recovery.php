<?php declare(strict_types=1);
function providerPrepareRecovery(FvsCore $core,array $a,string $secret):array{
    $pub=rtrim(getenv('FVS_PUBLIC_BASE_URL')?:'http://localhost:8080','/');
    $api=rtrim(getenv('FVS_API_BASE_URL')?:$pub,'/');
    $retry=max(5,min(3600,(int)(getenv('FVS_PAYMENT_RETRY_SECONDS')?:30)));
    try{
        $p=$a['provider']==='stripe'?stripePrepare($a):mpPrepare($a,$api.'/api/v1/webhooks/mercadopago',$pub.'/?checkout=return');
        $core->checkoutAttach($a['attempt_id'],$p['checkout_id'],$p['payment_id'],$p['client_secret'],$p['redirect_url']);
        $core->paymentRecoveryMark($a['attempt_id'],'active','provider_ready','',0);
        return publicAttempt($core->checkoutGet($a['cart_id'],$secret));
    }catch(ProviderException $x){
        $core->paymentRecoveryMark($a['attempt_id'],'provider_error','provider_prepare',get_class($x),$retry);
        throw $x;
    }
}
