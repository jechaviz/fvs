<?php declare(strict_types=1);
require dirname(__DIR__).'/shim-php/Payments.php';
require dirname(__DIR__).'/shim-php/Core.php';
require dirname(__DIR__).'/shim-php/InventoryImport.php';
function ck(bool $v,string $m):void{if(!$v){fwrite(STDERR,"FAIL $m\n");exit(1);}}
ck(minorFromString('1234.56')===123456,'money');
$secret='mp-secret';$rid='req-1';$did='123';$ts='1700000000';$manifest="id:$did;request-id:$rid;ts:$ts;";$sig=hash_hmac('sha256',$manifest,$secret);mpVerify("ts=$ts,v1=$sig",$rid,$did,$secret);
$raw='{"transaction_amount":1234.56}';ck(rawNumberMinor($raw,'transaction_amount')===123456,'raw amount');
$lib=getenv('FVS_CORE_LIB')?:dirname(__DIR__).'/core/build/release/libfvs_core.so';$ffi=FFI::cdef('const char *fvs_version(void); int fvs_uuid_v4(char out[37]);',$lib);$ver=$ffi->fvs_version();if(!is_string($ver))$ver=FFI::string($ver);ck(str_starts_with($ver,'6.0.0'),'ffi version');$probe=new FvsCore(false);ck(str_starts_with($probe->version(),'6.0.0'),'php liveness core version without DB');$u=FFI::new('char[37]');ck($ffi->fvs_uuid_v4($u)===0&&strlen(FFI::string($u))===36,'ffi uuid');
require dirname(__DIR__).'/workers/worker.php';
$redacted=redactSupportSensitive('Tarjeta 4111 1111 1111 1111 y CVV: 123');ck(!str_contains($redacted,'4111')&&!str_contains($redacted,'123')&&str_contains($redacted,'DATOS_DE_PAGO_REDACTADOS'),'php support payment-data redaction');
$fake=['order_id'=>'11111111-1111-4111-8111-111111111111','customer_email'=>'buyer@example.com','total_minor'=>640000,'currency'=>'MXN','items'=>[]];
for($i=0;$i<64;$i++)$fake['items'][]=['resort'=>'R','unit_name'=>'Suite','check_in'=>'2026-12-01','check_out'=>'2026-12-08','guests'=>2,'price_minor'=>10000,'currency'=>'MXN','week_number'=>49,'booking_mode'=>'fixed','float_group'=>null];
$pdf=pdf($fake);ck(str_starts_with($pdf,'%PDF-1.4')&&substr_count($pdf,'/Type /Page ')>=4,'php voucher multipage');
echo "PASS php shim helper/FFI/voucher tests\n";
