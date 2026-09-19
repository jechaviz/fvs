<?php declare(strict_types=1);

final class UnsupportedMediaTypeException extends RuntimeException {}

function minorFromString(string $s): int {
    $s = str_replace(['$', ',', ' '], '', $s);
    if (!preg_match('/^-?\d+(?:\.\d+)?$/', $s)) {
        throw new InvalidArgumentException('invalid price');
    }
    $neg = str_starts_with($s, '-');
    if ($neg) $s = substr($s, 1);
    [$w, $f] = array_pad(explode('.', $s, 2), 2, '');
    $f = str_pad(substr($f, 0, 3), 3, '0');
    $c = (int)substr($f, 0, 2);
    if ((int)$f[2] >= 5) $c++;
    $v = ((int)$w) * 100 + $c;
    return $neg ? -$v : $v;
}

function normalizeHeading(string $s): string {
    return trim((string)preg_replace('/[^a-z0-9]+/', '_', strtolower(trim($s))), '_');
}

function parseCsvRows(string $raw): array {
    $first = strtok($raw, "\r\n") ?: '';
    $delim = substr_count($first, "\t") > substr_count($first, ',')
        ? "\t"
        : (substr_count($first, ';') > substr_count($first, ',') ? ';' : ',');
    $f = fopen('php://temp', 'r+');
    if ($f === false) throw new RuntimeException('temporary stream unavailable');
    fwrite($f, $raw);
    rewind($f);
    $head = fgetcsv($f, 0, $delim);
    if (!$head) { fclose($f); return []; }
    $head = array_map('normalizeHeading', $head);
    $rows = [];
    while (($r = fgetcsv($f, 0, $delim)) !== false) {
        if (!array_filter($r, fn($v) => trim((string)$v) !== '')) continue;
        $r = array_slice(array_pad($r, count($head), ''), 0, count($head));
        $rows[] = array_combine($head, $r);
    }
    fclose($f);
    return $rows;
}

function xlsxColumnIndex(string $ref): int {
    if (!preg_match('/^([A-Z]+)/i', $ref, $m)) return 0;
    $n = 0;
    foreach (str_split(strtoupper($m[1])) as $ch) $n = $n * 26 + (ord($ch) - 64);
    return max(0, $n - 1);
}

function xlsxXml(string $xml): SimpleXMLElement {
    $old = libxml_use_internal_errors(true);
    try {
        $node = simplexml_load_string($xml, SimpleXMLElement::class, LIBXML_NONET | LIBXML_COMPACT);
        if ($node === false) throw new InvalidArgumentException('invalid XLSX XML');
        return $node;
    } finally {
        libxml_clear_errors();
        libxml_use_internal_errors($old);
    }
}

/** @return array<int,array<int,string>> */
function parseXlsxMatrix(string $raw): array {
    if (!class_exists('ZipArchive') || !function_exists('simplexml_load_string')) {
        throw new UnsupportedMediaTypeException('XLSX requires PHP extensions zip and SimpleXML');
    }
    $tmp = tempnam(sys_get_temp_dir(), 'fvs-xlsx-');
    if ($tmp === false) throw new RuntimeException('cannot allocate XLSX temporary file');
    try {
        if (file_put_contents($tmp, $raw, LOCK_EX) === false) throw new RuntimeException('cannot stage XLSX');
        $zip = new ZipArchive();
        if ($zip->open($tmp) !== true) throw new InvalidArgumentException('invalid XLSX archive');
        try {
            $shared = [];
            $sharedRaw = $zip->getFromName('xl/sharedStrings.xml');
            if ($sharedRaw !== false) {
                $root = xlsxXml($sharedRaw);
                $ns = $root->getNamespaces(true);
                $d = isset($ns['']) ? $root->children($ns['']) : $root;
                foreach ($d->si as $si) {
                    $parts = $si->xpath('.//*[local-name()="t"]') ?: [];
                    $text = '';
                    foreach ($parts as $p) $text .= (string)$p;
                    $shared[] = $text;
                }
            }
            $sheetName = null;
            for ($i = 0; $i < $zip->numFiles; $i++) {
                $name = $zip->getNameIndex($i);
                if ($name !== false && preg_match('#^xl/worksheets/sheet\d+\.xml$#', $name)) {
                    $sheetName = $name; break;
                }
            }
            if ($sheetName === null) throw new InvalidArgumentException('xlsx has no worksheet');
            $sheetRaw = $zip->getFromName($sheetName);
            if ($sheetRaw === false) throw new InvalidArgumentException('cannot read XLSX worksheet');
            $root = xlsxXml($sheetRaw);
            $rowNodes = $root->xpath('//*[local-name()="row"]') ?: [];
            $rows = [];
            foreach ($rowNodes as $rowNode) {
                $vals = [];
                $cells = $rowNode->xpath('./*[local-name()="c"]') ?: [];
                foreach ($cells as $cell) {
                    $attrs = $cell->attributes();
                    $idx = xlsxColumnIndex((string)($attrs['r'] ?? 'A'));
                    $type = (string)($attrs['t'] ?? '');
                    $vNodes = $cell->xpath('./*[local-name()="v"]') ?: [];
                    $tNodes = $cell->xpath('.//*[local-name()="t"]') ?: [];
                    $text = $tNodes ? (string)$tNodes[0] : ($vNodes ? (string)$vNodes[0] : '');
                    if ($type === 's' && $text !== '' && ctype_digit($text)) $text = $shared[(int)$text] ?? '';
                    $vals[$idx] = $text;
                }
                if ($vals) {
                    $max = max(array_keys($vals));
                    $line = [];
                    for ($i = 0; $i <= $max; $i++) $line[] = $vals[$i] ?? '';
                    $rows[] = $line;
                }
            }
            return $rows;
        } finally {
            $zip->close();
        }
    } finally {
        @unlink($tmp);
    }
}

function matrixToAssocRows(array $matrix): array {
    if (!$matrix) return [];
    $head = array_map(fn($v) => normalizeHeading((string)$v), array_shift($matrix));
    $rows = [];
    foreach ($matrix as $r) {
        if (!array_filter($r, fn($v) => trim((string)$v) !== '')) continue;
        $r = array_slice(array_pad($r, count($head), ''), 0, count($head));
        $rows[] = array_combine($head, $r);
    }
    return $rows;
}

function normRow(array $r): array {
    foreach (['resort', 'unit_code', 'unit_name', 'check_in', 'check_out'] as $k) {
        if (trim((string)($r[$k] ?? '')) === '') throw new InvalidArgumentException('missing '.$k);
    }
    $src = trim((string)($r['source_ref'] ?? ''));
    if ($src === '') {
        $src = 'auto:'.substr(hash('sha256', implode('|', [$r['resort'], $r['unit_code'], $r['check_in'], $r['check_out']])), 0, 40);
    }
    $pm = trim((string)($r['price_minor'] ?? '')) !== ''
        ? (int)str_replace(',', '', (string)$r['price_minor'])
        : minorFromString((string)($r['price'] ?? ''));
    $active = !in_array(strtolower(trim((string)($r['active'] ?? '1'))), ['0', 'false', 'no', 'inactive'], true);
    $mode = strtolower(trim((string)($r['booking_mode'] ?? 'fixed')));
    if (!in_array($mode, ['fixed','floating'], true)) throw new InvalidArgumentException('invalid booking_mode');
    $floatGroup = trim((string)($r['float_group'] ?? ''));
    if ($mode === 'floating' && $floatGroup === '') throw new InvalidArgumentException('floating inventory requires float_group');
    $weekRaw = trim((string)($r['week_number'] ?? ''));
    $week = $weekRaw !== '' ? (int)$weekRaw : (int)(new DateTimeImmutable(substr(trim((string)$r['check_in']),0,10)))->format('W');
    if ($week < 1 || $week > 53) throw new InvalidArgumentException('invalid week_number');
    return [
        'source_ref' => $src,
        'resort' => trim((string)$r['resort']),
        'unit_code' => trim((string)$r['unit_code']),
        'unit_name' => trim((string)$r['unit_name']),
        'city' => trim((string)($r['city'] ?? '')),
        'country' => trim((string)($r['country'] ?? '')),
        'check_in' => substr(trim((string)$r['check_in']), 0, 10),
        'check_out' => substr(trim((string)$r['check_out']), 0, 10),
        'week_number' => $week,
        'booking_mode' => $mode,
        'float_group' => $floatGroup,
        'max_guests' => (int)($r['max_guests'] ?? 2),
        'price_minor' => $pm,
        'currency' => strtoupper(trim((string)($r['currency'] ?? 'MXN'))),
        'image_url' => trim((string)($r['image_url'] ?? '')),
        'short_description' => trim((string)($r['short_description'] ?? '')),
        'season' => trim((string)($r['season'] ?? '')),
        'latitude' => trim((string)($r['latitude'] ?? '')),
        'longitude' => trim((string)($r['longitude'] ?? '')),
        'property_type' => trim((string)($r['property_type'] ?? '')),
        'bedrooms' => (int)($r['bedrooms'] ?? 0),
        'bathrooms' => (float)($r['bathrooms'] ?? 0),
        'rating' => (float)($r['rating'] ?? 0),
        'amenities' => array_values(array_filter(array_map('trim', preg_split('/[,;|]/', (string)($r['amenities'] ?? '')) ?: []))),
        'active' => $active,
    ];
}

function importInventoryBytes(FvsCore $core, string $raw, string $filename): array {
    $ext = strtolower(pathinfo($filename, PATHINFO_EXTENSION));
    if (in_array($ext, ['csv', 'tsv', 'txt'], true)) {
        $rows = parseCsvRows($raw);
    } elseif (in_array($ext, ['xlsx', 'xlsm'], true)) {
        $rows = matrixToAssocRows(parseXlsxMatrix($raw));
    } else {
        throw new UnsupportedMediaTypeException('Supported inventory files: CSV, TSV, XLSX, XLSM');
    }
    $rep = ['rows_seen' => count($rows), 'inserted' => 0, 'updated' => 0, 'rejected' => 0, 'errors' => []];
    foreach ($rows as $i => $r) {
        try {
            $created = $core->inventoryUpsert(normRow($r));
            $rep[$created ? 'inserted' : 'updated']++;
        } catch (Throwable $x) {
            $rep['rejected']++;
            if (count($rep['errors']) < 50) $rep['errors'][] = ['row' => $i + 2, 'error' => substr($x->getMessage(), 0, 240)];
        }
    }
    return $rep;
}
