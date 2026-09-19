from __future__ import annotations
import hashlib, html, json, os, re, secrets

PUBLIC_BASE=os.getenv('FVS_PUBLIC_BASE_URL','http://localhost:8080').rstrip('/')

def agent_ok(env):
    supplied=env.get('HTTP_X_AGENT_KEY','')
    expected_hash=os.getenv('FVS_AGENT_KEY_SHA256','').strip().lower()
    if expected_hash:
        return secrets.compare_digest(hashlib.sha256(supplied.encode('utf-8')).hexdigest(),expected_hash)
    if os.getenv('FVS_ENV','development')=='production': return False
    expected=os.getenv('FVS_AGENT_KEY','')
    return bool(expected and secrets.compare_digest(supplied,expected))

def agent_id(env):
    raw=(env.get('HTTP_X_AGENT_ID') or '').strip()
    return raw[:64] if re.fullmatch(r'[A-Za-z0-9._:@-]{1,64}',raw) else ''

def seo_html(page):
    e=lambda v:html.escape(str(v or ''),quote=True)
    prop=page.get('property') or {}
    canonical=PUBLIC_BASE+str(page.get('canonical_path') or '')
    image=str(page.get('og_image_url') or prop.get('image_url') or '')
    faq=page.get('faq') if isinstance(page.get('faq'),list) else []
    page_type=str(page.get('page_type') or 'property')

    if page_type == 'property':
        accommodation={
            '@type':'Accommodation',
            'name':prop.get('unit_name') or page.get('h1'),
            'identifier':prop.get('slot_id') or None,
        }
        if prop.get('max_guests'):
            accommodation['occupancy']={'@type':'QuantitativeValue','maxValue':int(prop.get('max_guests') or 0)}
        ld={
            '@context':'https://schema.org',
            '@type':'LodgingBusiness',
            'name':prop.get('resort') or page.get('h1'),
            'description':page.get('meta_description'),
            'url':canonical,
            'containsPlace':accommodation,
        }
        if image: ld['image']=[image]
        if prop.get('city') or prop.get('country'):
            ld['address']={'@type':'PostalAddress','addressLocality':prop.get('city') or '','addressCountry':prop.get('country') or ''}
        if prop.get('price_minor') is not None and prop.get('currency'):
            ld['offers']={
                '@type':'Offer',
                'price':f"{int(prop.get('price_minor') or 0)/100:.2f}",
                'priceCurrency':str(prop.get('currency')),
                'availability':'https://schema.org/InStock',
                'url':canonical,
                'itemOffered':accommodation,
            }
    else:
        schema_type='CollectionPage' if page_type in ('collection','destination') else 'WebPage'
        ld={
            '@context':'https://schema.org','@type':schema_type,
            'name':page.get('h1') or page.get('title'),
            'description':page.get('meta_description'),'url':canonical,
        }
        if image: ld['primaryImageOfPage']={'@type':'ImageObject','url':image}

    breadcrumb={'@context':'https://schema.org','@type':'BreadcrumbList','itemListElement':[{'@type':'ListItem','position':1,'name':'FVS','item':PUBLIC_BASE+'/'},{'@type':'ListItem','position':2,'name':page.get('h1'),'item':canonical}]}
    jsonlds=''.join('<script type="application/ld+json">'+json.dumps(x,ensure_ascii=False,separators=(',',':')).replace('</','<\\/')+'</script>' for x in (ld,breadcrumb))
    faq_html=''.join('<details><summary>'+e(x.get('question'))+'</summary><p>'+e(x.get('answer'))+'</p></details>' for x in faq if isinstance(x,dict) and x.get('question') and x.get('answer'))
    og_image='<meta property="og:image" content="'+e(image)+'">' if image else ''
    hero='<img class="seo-hero" src="'+e(image)+'" alt="'+e(page.get('h1'))+'" loading="eager" fetchpriority="high">' if image else ''
    faq_section='<section class="seo-faq"><h2>Preguntas frecuentes</h2>'+faq_html+'</section>' if faq_html else ''
    price=''
    if prop.get('price_minor') is not None and prop.get('currency'):
        price=f"{int(prop.get('price_minor') or 0)/100:.2f} {prop.get('currency') or ''}"
    robots='index,follow,max-image-preview:large' if page.get('indexable',True) else 'noindex,follow'
    property_facts=''
    property_cta=''
    eyebrow=e(prop.get('city') or prop.get('country'))
    if page_type == 'property':
        property_facts=(
            '<dl class="seo-facts"><div><dt>Entrada</dt><dd>'+e(prop.get('check_in'))+'</dd></div>'
            '<div><dt>Salida</dt><dd>'+e(prop.get('check_out'))+'</dd></div>'
            '<div><dt>Huéspedes</dt><dd>'+e(prop.get('max_guests'))+'</dd></div>'
            '<div><dt>Precio</dt><dd>'+e(price)+'</dd></div></dl>'
        )
        property_cta='<p><a class="primary seo-cta" href="/?slot='+e(prop.get('slot_id'))+'&utm_source=organic&utm_medium=seo&utm_campaign=property_page">Ver disponibilidad y reservar</a></p>'
    return (
        '<!doctype html><html lang="'+e(page.get('locale') or 'es-MX')+'"><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width,initial-scale=1"><title>'+e(page.get('title'))+'</title>'
        '<meta name="description" content="'+e(page.get('meta_description'))+'"><meta name="robots" content="'+robots+'">'
        '<link rel="canonical" href="'+e(canonical)+'"><meta property="og:type" content="website">'
        '<meta property="og:title" content="'+e(page.get('title'))+'"><meta property="og:description" content="'+e(page.get('meta_description'))+'">'
        '<meta property="og:url" content="'+e(canonical)+'">'+og_image+'<meta name="twitter:card" content="summary_large_image">'
        '<link rel="stylesheet" href="/css/base.css">'+jsonlds+'</head><body>'
        '<main class="shell"><header class="topbar"><a class="brand" href="/">FVS<small>Vacation Store</small></a></header>'
        '<article class="seo-page"><p class="eyebrow">'+eyebrow+'</p><h1>'+e(page.get('h1'))+'</h1>'+hero+
        '<p class="seo-lead">'+e(page.get('body_text') or page.get('meta_description'))+'</p>'+property_facts+property_cta+faq_section+
        '</article></main></body></html>'
    )

def sitemap_xml(pages):
    items=[]
    for p in pages:
        loc=PUBLIC_BASE+str(p.get('canonical_path') or '')
        items.append('<url><loc>'+html.escape(loc)+'</loc><lastmod>'+html.escape(str(p.get('updated_at') or '')[:10])+'</lastmod><changefreq>'+html.escape(str(p.get('changefreq') or 'weekly'))+'</changefreq><priority>'+f"{float(p.get('priority') or .7):.2f}"+'</priority></url>')
    return '<?xml version="1.0" encoding="UTF-8"?><urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">'+''.join(items)+'</urlset>'
