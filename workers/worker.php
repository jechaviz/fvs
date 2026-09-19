<?php declare(strict_types=1);
require_once dirname(__DIR__).'/shim-php/Core.php';

$dir=getenv('FVS_VOUCHER_DIR')?:dirname(__DIR__).'/var/vouchers';
if(!is_dir($dir) && !mkdir($dir,0770,true) && !is_dir($dir)) throw new RuntimeException('cannot create voucher directory');
$ownerPrefix='php:'.gethostname().':'.getmypid();

function pe(string $s):string{return str_replace(['\\','(',')'],['\\\\','\\(','\\)'],$s);}
function moneyMinor(int $v):string{$sign=$v<0?'-':'';$n=abs($v);return $sign.intdiv($n,100).'.'.str_pad((string)($n%100),2,'0',STR_PAD_LEFT);}
function pdf(array $o):string{
    $lines=['FVS - Confirmacion de reserva','Orden: '.$o['order_id'],'Cliente: '.$o['customer_email'],'Total: '.moneyMinor((int)$o['total_minor']).' '.$o['currency'],''];
    foreach($o['items']??[] as $i=>$it){
        $mode=strtolower((string)($it['booking_mode']??'fixed'));$week=$it['week_number']??null;
        $label=($week!==null&&$week!==''?'Semana '.$week.' - ':'').($mode==='floating'?'Flotante':'Fija');
        if($mode==='floating'&&!empty($it['float_group']))$label.=' - Grupo '.$it['float_group'];
        $lines[]=($i+1).'. '.$it['resort'].' - '.$it['unit_name'];
        $lines[]='   '.$it['check_in'].' a '.$it['check_out'].' - '.$label;
        $lines[]='   '.$it['guests'].' huespedes - '.moneyMinor((int)$it['price_minor']).' '.$it['currency'];
    }
    $perPage=42;$chunks=array_chunk($lines,$perPage);if(!$chunks)$chunks=[[]];$n=count($chunks);$fontObj=3+2*$n;
    $kids=[];for($i=0;$i<$n;$i++)$kids[]=(3+2*$i).' 0 R';
    $objs=['<< /Type /Catalog /Pages 2 0 R >>','<< /Type /Pages /Kids ['.implode(' ',$kids).'] /Count '.$n.' >>'];
    foreach($chunks as $idx=>$chunk){$pageNo=$idx+1;$pageObj=3+2*$idx;$contentObj=$pageObj+1;$stream="BT /F1 11 Tf 48 760 Td 14 TL\n";foreach($chunk as $l)$stream.='('.pe((string)$l).") Tj T*\n";$stream.='(Pagina '.$pageNo.'/'.$n.") Tj\nET";$objs[]='<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 '.$fontObj.' 0 R >> >> /Contents '.$contentObj.' 0 R >>';$objs[]='<< /Length '.strlen($stream)." >>\nstream\n$stream\nendstream";}
    $objs[]='<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>';
    $out="%PDF-1.4\n";$offs=[0];foreach($objs as $i=>$obj){$offs[]=strlen($out);$num=$i+1;$out.="$num 0 obj\n$obj\nendobj\n";}$xref=strlen($out);$out.='xref'."\n0 ".(count($objs)+1)."\n0000000000 65535 f \n";for($i=1;$i<count($offs);$i++)$out.=sprintf("%010d 00000 n \n",$offs[$i]);$out.='trailer << /Size '.(count($objs)+1)." /Root 1 0 R >>\nstartxref\n$xref\n%%EOF\n";return $out;
}

function smtpRead($fp):array{
    $lines=[];$code=0;
    do{$line=fgets($fp,4096);if($line===false)throw new RuntimeException('SMTP connection closed');$lines[]=$line;if(preg_match('/^(\d{3})([ -])/',$line,$m)){$code=(int)$m[1];$more=$m[2]==='-';}else{$more=false;}}while($more);
    return [$code,implode('',$lines)];
}
function smtpCmd($fp,string $cmd,array $ok):void{
    if($cmd!=='') { if(fwrite($fp,$cmd."\r\n")===false)throw new RuntimeException('SMTP write failed'); }
    [$code]=smtpRead($fp);if(!in_array($code,$ok,true))throw new RuntimeException('SMTP command rejected: '.$code);
}
function smtpSend(array $recipients,string $from,string $rawMessage):void{
    $host=getenv('FVS_SMTP_HOST')?:'';if($host==='')throw new RuntimeException('FVS_SMTP_HOST missing');$port=(int)(getenv('FVS_SMTP_PORT')?:587);
    $fp=@stream_socket_client('tcp://'.$host.':'.$port,$errno,$errstr,15,STREAM_CLIENT_CONNECT);if($fp===false)throw new RuntimeException('SMTP connect failed');stream_set_timeout($fp,15);
    try{
        smtpCmd($fp,'',[220]);$ehlo=preg_replace('/[^A-Za-z0-9.-]/','',gethostname()?:'fvs')?:'fvs';smtpCmd($fp,'EHLO '.$ehlo,[250]);
        if((getenv('FVS_SMTP_STARTTLS')?:'1')==='1'){
            smtpCmd($fp,'STARTTLS',[220]);if(!stream_socket_enable_crypto($fp,true,STREAM_CRYPTO_METHOD_TLS_CLIENT))throw new RuntimeException('SMTP TLS failed');smtpCmd($fp,'EHLO '.$ehlo,[250]);
        }
        $user=getenv('FVS_SMTP_USER')?:'';if($user!==''){
            smtpCmd($fp,'AUTH LOGIN',[334]);smtpCmd($fp,base64_encode($user),[334]);smtpCmd($fp,base64_encode(getenv('FVS_SMTP_PASSWORD')?:''),[235]);
        }
        smtpCmd($fp,'MAIL FROM:<'.$from.'>',[250]);foreach($recipients as $recipient)smtpCmd($fp,'RCPT TO:<'.$recipient.'>',[250,251]);smtpCmd($fp,'DATA',[354]);
        $wire=preg_replace('/(?m)^\./','..',str_replace(["\r\n","\r"],"\n",$rawMessage));$wire=str_replace("\n","\r\n",$wire);if(fwrite($fp,$wire."\r\n.\r\n")===false)throw new RuntimeException('SMTP data write failed');[$code]=smtpRead($fp);if($code!==250)throw new RuntimeException('SMTP message rejected: '.$code);smtpCmd($fp,'QUIT',[221]);
    }finally{fclose($fp);}
}
function mimeMessage(array $o,string $path,string $from):array{
    $oid=(string)$o['order_id'];$to=preg_replace('/[\r\n]+/','',(string)$o['customer_email']);$from=preg_replace('/[\r\n]+/','',$from);if(!filter_var($to,FILTER_VALIDATE_EMAIL)||!filter_var($from,FILTER_VALIDATE_EMAIL))throw new RuntimeException('invalid email envelope');
    $cc=[];foreach(explode(',',getenv('FVS_EMAIL_CC')?:'') as $raw){$addr=trim($raw);if($addr==='')continue;if(preg_match('/[\r\n]/',$addr)||!filter_var($addr,FILTER_VALIDATE_EMAIL))throw new RuntimeException('invalid FVS_EMAIL_CC address');$cc[]=$addr;}
    $boundary='=_fvs_'.bin2hex(random_bytes(12));$filename=basename($path);$attachment=chunk_split(base64_encode((string)file_get_contents($path)));$subject='FVS - Reserva '.$oid;
    $domain=preg_replace('/[^A-Za-z0-9.-]/','',getenv('FVS_MESSAGE_ID_DOMAIN')?:'fvs.local')?:'fvs.local';$headers="From: $from\r\nTo: $to\r\n".($cc?'Cc: '.implode(', ',$cc)."\r\n":'')."Subject: $subject\r\nDate: ".date(DATE_RFC2822)."\r\nMessage-ID: <fvs-order-$oid@$domain>\r\nX-FVS-Order-ID: $oid\r\nMIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary=\"$boundary\"";
    $body="--$boundary\r\nContent-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: 8bit\r\n\r\nTu reserva FVS $oid fue confirmada. Adjuntamos tu voucher.\r\n--$boundary\r\nContent-Type: application/pdf; name=\"$filename\"\r\nContent-Transfer-Encoding: base64\r\nContent-Disposition: attachment; filename=\"$filename\"\r\n\r\n$attachment\r\n--$boundary--\r\n";
    return [$to,$cc,$from,$subject,$headers,$body];
}

function redactSupportSensitive(string $text):string{
    $text=preg_replace('/\b(?:cvv|cvc|código de seguridad|codigo de seguridad)\s*[:=]?\s*\d{3,4}\b/iu','[DATOS_DE_PAGO_REDACTADOS]',$text)??$text;
    return preg_replace('/(?<!\d)(?:\d[ -]?){13,19}(?!\d)/','[DATOS_DE_PAGO_REDACTADOS]',$text)??$text;
}

function supportAiCall(array $thread):array{
    $key=getenv('OPENAI_API_KEY')?:'';if($key==='')throw new RuntimeException('OPENAI_API_KEY missing');$model=getenv('FVS_SUPPORT_AI_MODEL')?:'gpt-5.6-luna';$messages=[];
    foreach(array_slice($thread['messages']??[],-24) as $m){if(($m['visibility']??'public')!=='public')continue;$role=in_array($m['sender_type']??'',['ai','agent'],true)?'assistant':'user';$messages[]=['role'=>$role,'content'=>substr(redactSupportSensitive((string)($m['body']??'')),0,8000)];}
    $system='Eres el asistente de soporte de FVS, una plataforma de reservaciones vacacionales. Responde en el idioma del huésped, breve y con precisión. No inventes disponibilidad, precios, políticas ni estados de pago. No ejecutes cancelaciones, reembolsos, cambios de reserva ni decisiones financieras. Para esas acciones, o si falta información crítica, responde con el prefijo [HANDOFF] y una razón corta. Nunca pidas números completos de tarjeta, CVV, contraseñas o secretos. Escala fraude, cargos no reconocidos, emergencia o disputa.';
    $kb=getenv('FVS_SUPPORT_KB_PATH')?:'';if($kb!==''&&is_file($kb))$system.="\nBase de conocimiento aprobada:\n".substr((string)file_get_contents($kb),0,120000);
    $payload=json_encode(['model'=>$model,'store'=>false,'instructions'=>$system,'input'=>$messages,'max_output_tokens'=>700],JSON_UNESCAPED_UNICODE|JSON_UNESCAPED_SLASHES|JSON_THROW_ON_ERROR);
    $ch=curl_init('https://api.openai.com/v1/responses');curl_setopt_array($ch,[CURLOPT_POST=>true,CURLOPT_POSTFIELDS=>$payload,CURLOPT_HTTPHEADER=>['Authorization: Bearer '.$key,'Content-Type: application/json','User-Agent: FVS-support/4.0'],CURLOPT_RETURNTRANSFER=>true,CURLOPT_TIMEOUT=>(int)(getenv('FVS_SUPPORT_AI_TIMEOUT')?:25)]);$raw=curl_exec($ch);if($raw===false){$e=curl_error($ch);curl_close($ch);throw new RuntimeException('OpenAI transport '.$e);}$code=(int)curl_getinfo($ch,CURLINFO_RESPONSE_CODE);curl_close($ch);if($code<200||$code>=300)throw new RuntimeException('OpenAI HTTP '.$code);$r=json_decode($raw,true,512,JSON_THROW_ON_ERROR);$parts=[];if(!empty($r['output_text']))$parts[]=(string)$r['output_text'];foreach($r['output']??[] as $o)foreach($o['content']??[] as $c)if(in_array($c['type']??'',['output_text','text'],true)&&isset($c['text']))$parts[]=(string)$c['text'];$text=trim(implode("\n",array_unique($parts)));if($text==='')throw new RuntimeException('empty AI response');return [$text,$model,(int)($r['usage']['input_tokens']??0),(int)($r['usage']['output_tokens']??0)];
}
function processSupportAi(FvsCore $c,array $e):void{$tid=(string)($e['payload']['thread_id']??$e['aggregate_id']??'');if((getenv('FVS_SUPPORT_AI_ENABLED')?:'1')!=='1'){$c->supportHandoff($tid,'Asistencia IA deshabilitada; continuar con agente humano');return;}$t=$c->supportThreadContext($tid);if(empty($t['ai_enabled']))return;$public=array_values(array_filter($t['messages']??[],fn($m)=>(($m['visibility']??'public')==='public')));$latest=(string)($public[count($public)-1]['body']??'');if(preg_match('/\b(reembolso|refund|cancel(?:ar|ación)?|chargeback|fraude|fraud|emergencia|emergency|abogado|legal|demanda|dispute|cargo no reconocido|modificar reserva|cambiar reserva)\b/iu',$latest)){$c->supportHandoff($tid,'Solicitud sensible o acción que requiere agente humano');return;}[$text,$model,$in,$out]=supportAiCall($t);if(str_starts_with($text,'[HANDOFF]')){$c->supportHandoff($tid,substr(trim(substr($text,9)),0,500)?:'Escalamiento solicitado por IA');return;}$c->supportAiMessage($tid,$text,$model,$in,$out);}

function processEvent(FvsCore $c,array $e,string $dir):void{
    if(($e['event_type']??'')==='support.ai_requested'){processSupportAi($c,$e);return;}
    if(($e['event_type']??'')!=='order.confirmed')throw new RuntimeException('unsupported outbox event type');$oid=$e['payload']['order_id']??$e['aggregate_id'];$o=$c->orderGet($oid);$path=$dir.'/'.$oid.'.pdf';$tmp=$path.'.'.getmypid().'.'.bin2hex(random_bytes(4)).'.tmp';if(file_put_contents($tmp,pdf($o),LOCK_EX)===false)throw new RuntimeException('voucher write failed');if(!rename($tmp,$path)){@unlink($tmp);throw new RuntimeException('voucher atomic rename failed');}
    $mode=strtolower(getenv('FVS_EMAIL_MODE')?:'log');if($mode==='log'){echo "EMAIL(log) to={$o['customer_email']} voucher=$path\n";return;}$from=getenv('FVS_SMTP_FROM')?:'reservas@localhost';[$to,$cc,$from,$subject,$headers,$body]=mimeMessage($o,$path,$from);
    if($mode==='smtp'){smtpSend(array_merge([$to],$cc),$from,$headers."\r\n\r\n".$body);return;}if($mode==='mail'){if(!mail($to,$subject,$body,$headers))throw new RuntimeException('mail failed');return;}throw new RuntimeException('unsupported FVS_EMAIL_MODE');
}

function workerLoop(FvsCore $core,string $dir,string $ownerPrefix):never{$idle=(float)(getenv('FVS_WORKER_IDLE_SECONDS')?:2);$sweepEvery=max(30,(float)(getenv('FVS_MAINTENANCE_INTERVAL_SECONDS')?:60));$heartbeatEvery=max(5,(float)(getenv('FVS_WORKER_HEARTBEAT_SECONDS')?:15));$cartTtl=(int)(getenv('FVS_OPEN_CART_TTL_SECONDS')?:86400);$nextSweep=0.0;$nextHeartbeat=0.0;$stop=false;if(function_exists('pcntl_async_signals')){pcntl_async_signals(true);pcntl_signal(SIGTERM,function()use(&$stop){$stop=true;});pcntl_signal(SIGINT,function()use(&$stop){$stop=true;});}while(!$stop){$now=microtime(true);if($now>=$nextHeartbeat){try{$core->workerHeartbeat('outbox',$ownerPrefix);}catch(Throwable $x){error_log('FVS heartbeat '.get_class($x));}$nextHeartbeat=$now+$heartbeatEvery;}if($now>=$nextSweep){try{$m=$core->maintenanceSweep($cartTtl);if(($m['expired_carts']??0)||($m['released_holds']??0))echo 'MAINTENANCE expired_carts='.$m['expired_carts'].' released_holds='.$m['released_holds']."\n";}catch(Throwable $x){error_log('FVS maintenance '.get_class($x));}$nextSweep=$now+$sweepEvery;}$owner=$ownerPrefix.':'.bin2hex(random_bytes(8));$lease=max(30,min(3600,(int)(getenv('FVS_OUTBOX_LEASE_SECONDS')?:300)));$e=$core->outboxClaim($owner,$lease);if(!$e){usleep((int)(min($idle,1.0)*1000000));continue;}$id=$e['event_id'];$token=(string)($e['lease_token']??'');try{processEvent($core,$e,$dir);$core->outboxAck($id,$owner,$token);}catch(Throwable $x){error_log('FVS worker '.$id.' '.get_class($x));try{$core->outboxNack($id,$owner,$token,get_class($x));}catch(Throwable){}}}try{$core->workerGoodbye('outbox',$ownerPrefix);}catch(Throwable $x){error_log('FVS goodbye '.get_class($x));}echo "worker shutdown complete\n";exit(0);}
if(realpath($_SERVER['SCRIPT_FILENAME']??'')===__FILE__){workerLoop(new FvsCore(),$dir,$ownerPrefix);}
