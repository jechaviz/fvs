<?php declare(strict_types=1);

function agentOk(): bool {
    $sup = $_SERVER['HTTP_X_AGENT_KEY'] ?? '';
    $hash = strtolower(trim((string)(getenv('FVS_AGENT_KEY_SHA256') ?: '')));
    if ($hash !== '') return hash_equals($hash, hash('sha256', $sup));
    if ((getenv('FVS_ENV') ?: 'development') === 'production') return false;
    $expected = getenv('FVS_AGENT_KEY') ?: '';
    return $expected !== '' && hash_equals($expected, $sup);
}

function agentId(): string {
    $v = trim((string)($_SERVER['HTTP_X_AGENT_ID'] ?? ''));
    return preg_match('/^[A-Za-z0-9._:@-]{1,64}$/D', $v) ? $v : '';
}

function supportRespond(int $code, array $data, string $secret): never {
    http_response_code($code);
    header('Content-Type: application/json; charset=utf-8');
    header('Cache-Control: no-store');
    setcookie('fvs_support_secret', $secret, [
        'expires' => time() + 604800,
        'path' => '/api/v1/support',
        'secure' => getenv('FVS_COOKIE_SECURE') === '1',
        'httponly' => true,
        'samesite' => 'Lax',
    ]);
    echo json_encode($data, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
    exit;
}

function h(mixed $v): string {
    return htmlspecialchars((string)($v ?? ''), ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8');
}

function seoHtml(array $p): string {
    $base = rtrim(getenv('FVS_PUBLIC_BASE_URL') ?: 'http://localhost:8080', '/');
    $prop = $p['property'] ?? [];
    $canonical = $base . (string)($p['canonical_path'] ?? '');
    $image = (string)($p['og_image_url'] ?? $prop['image_url'] ?? '');
    $pageType = (string)($p['page_type'] ?? 'property');
    $faq = is_array($p['faq'] ?? null) ? $p['faq'] : [];

    if ($pageType === 'property') {
        $accommodation = [
            '@type' => 'Accommodation',
            'name' => $prop['unit_name'] ?? $p['h1'] ?? '',
        ];
        if (!empty($prop['slot_id'])) $accommodation['identifier'] = $prop['slot_id'];
        if (!empty($prop['max_guests'])) {
            $accommodation['occupancy'] = ['@type' => 'QuantitativeValue', 'maxValue' => (int)$prop['max_guests']];
        }
        $ld = [
            '@context' => 'https://schema.org',
            '@type' => 'LodgingBusiness',
            'name' => $prop['resort'] ?? $p['h1'] ?? '',
            'description' => $p['meta_description'] ?? '',
            'url' => $canonical,
            'containsPlace' => $accommodation,
        ];
        if ($image !== '') $ld['image'] = [$image];
        if (($prop['city'] ?? '') !== '' || ($prop['country'] ?? '') !== '') {
            $ld['address'] = ['@type' => 'PostalAddress', 'addressLocality' => $prop['city'] ?? '', 'addressCountry' => $prop['country'] ?? ''];
        }
        if (array_key_exists('price_minor', $prop) && !empty($prop['currency'])) {
            $ld['offers'] = [
                '@type' => 'Offer',
                'price' => number_format(((int)$prop['price_minor']) / 100, 2, '.', ''),
                'priceCurrency' => $prop['currency'],
                'availability' => 'https://schema.org/InStock',
                'url' => $canonical,
                'itemOffered' => $accommodation,
            ];
        }
    } else {
        $ld = [
            '@context' => 'https://schema.org',
            '@type' => in_array($pageType, ['collection', 'destination'], true) ? 'CollectionPage' : 'WebPage',
            'name' => $p['h1'] ?? $p['title'] ?? '',
            'description' => $p['meta_description'] ?? '',
            'url' => $canonical,
        ];
        if ($image !== '') $ld['primaryImageOfPage'] = ['@type' => 'ImageObject', 'url' => $image];
    }

    $bc = [
        '@context' => 'https://schema.org', '@type' => 'BreadcrumbList',
        'itemListElement' => [
            ['@type' => 'ListItem', 'position' => 1, 'name' => 'FVS', 'item' => $base . '/'],
            ['@type' => 'ListItem', 'position' => 2, 'name' => $p['h1'] ?? '', 'item' => $canonical],
        ],
    ];
    $jsonld = '';
    foreach ([$ld, $bc] as $obj) {
        $encoded = json_encode($obj, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
        $jsonld .= '<script type="application/ld+json">' . str_replace('</', '<\/', (string)$encoded) . '</script>';
    }
    $faqHtml = '';
    foreach ($faq as $x) {
        if (is_array($x) && !empty($x['question']) && !empty($x['answer'])) {
            $faqHtml .= '<details><summary>' . h($x['question']) . '</summary><p>' . h($x['answer']) . '</p></details>';
        }
    }
    $price = '';
    if (array_key_exists('price_minor', $prop) && !empty($prop['currency'])) {
        $price = number_format(((int)$prop['price_minor']) / 100, 2, '.', '') . ' ' . h($prop['currency']);
    }
    $robots = !array_key_exists('indexable', $p) || $p['indexable'] ? 'index,follow,max-image-preview:large' : 'noindex,follow';
    $facts = '';
    $cta = '';
    if ($pageType === 'property') {
        $facts = '<dl class="seo-facts"><div><dt>Entrada</dt><dd>' . h($prop['check_in'] ?? '') . '</dd></div>'
            . '<div><dt>Salida</dt><dd>' . h($prop['check_out'] ?? '') . '</dd></div>'
            . '<div><dt>Huéspedes</dt><dd>' . h($prop['max_guests'] ?? '') . '</dd></div>'
            . '<div><dt>Precio</dt><dd>' . $price . '</dd></div></dl>';
        $cta = '<p><a class="primary seo-cta" href="/?slot=' . h($prop['slot_id'] ?? '') . '&amp;utm_source=organic&amp;utm_medium=seo&amp;utm_campaign=property_page">Ver disponibilidad y reservar</a></p>';
    }
    $hero = $image !== '' ? '<img class="seo-hero" src="' . h($image) . '" alt="' . h($p['h1'] ?? '') . '" loading="eager" fetchpriority="high">' : '';
    $faqSection = $faqHtml !== '' ? '<section class="seo-faq"><h2>Preguntas frecuentes</h2>' . $faqHtml . '</section>' : '';

    return '<!doctype html><html lang="' . h($p['locale'] ?? 'es-MX') . '"><head><meta charset="utf-8">'
        . '<meta name="viewport" content="width=device-width,initial-scale=1"><title>' . h($p['title'] ?? '') . '</title>'
        . '<meta name="description" content="' . h($p['meta_description'] ?? '') . '"><meta name="robots" content="' . h($robots) . '">'
        . '<link rel="canonical" href="' . h($canonical) . '"><meta property="og:type" content="website"><meta property="og:title" content="' . h($p['title'] ?? '') . '">'
        . '<meta property="og:description" content="' . h($p['meta_description'] ?? '') . '"><meta property="og:url" content="' . h($canonical) . '">'
        . ($image !== '' ? '<meta property="og:image" content="' . h($image) . '">' : '')
        . '<meta name="twitter:card" content="summary_large_image"><link rel="stylesheet" href="/css/base.css">' . $jsonld . '</head><body>'
        . '<main class="shell"><header class="topbar"><a class="brand" href="/">FVS<small>Vacation Store</small></a></header>'
        . '<article class="seo-page"><p class="eyebrow">' . h($prop['city'] ?? $prop['country'] ?? '') . '</p><h1>' . h($p['h1'] ?? '') . '</h1>' . $hero
        . '<p class="seo-lead">' . h($p['body_text'] ?? $p['meta_description'] ?? '') . '</p>' . $facts . $cta . $faqSection
        . '</article></main></body></html>';
}

function sitemapXml(array $pages): string {
    $base = rtrim(getenv('FVS_PUBLIC_BASE_URL') ?: 'http://localhost:8080', '/');
    $out = '';
    foreach ($pages as $p) {
        $out .= '<url><loc>' . h($base . ($p['canonical_path'] ?? '')) . '</loc><lastmod>' . h(substr((string)($p['updated_at'] ?? ''), 0, 10)) . '</lastmod>'
            . '<changefreq>' . h($p['changefreq'] ?? 'weekly') . '</changefreq><priority>' . number_format((float)($p['priority'] ?? .7), 2, '.', '') . '</priority></url>';
    }
    return '<?xml version="1.0" encoding="UTF-8"?><urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">' . $out . '</urlset>';
}
