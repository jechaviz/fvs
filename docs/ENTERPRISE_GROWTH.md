# Growth, SEO y soporte híbrido

## Objetivo

FVS usa el core C/MySQL y expone tres dominios productivos sin duplicar reglas entre PHP y Python:

1. soporte en línea IA → agente humano;
2. SEO técnico/programático;
3. growth y publicidad automatizada con guardrails.

## Soporte híbrido

El huésped abre un hilo persistente desde el catálogo. MySQL conserva el hilo, mensajes, estado, SLA, asignación y auditoría. El secreto del hilo viaja en cookie HttpOnly y se almacena hasheado.

Un mensaje del huésped puede generar `support.ai_requested` en el outbox. El worker consulta el contexto y usa OpenAI Responses API cuando `FVS_SUPPORT_AI_ENABLED=1`. Acciones sensibles no son ejecutadas por IA: la política fuerza handoff a humano.

La consola de agentes soporta presencia, cola por prioridad, claim exclusivo, nota interna, respuesta pública y resolución. El contexto previo IA/cliente se conserva para evitar que el huésped repita el caso.

## SEO

- páginas SSR bajo `/stay/<slug>`;
- canonical, robots, Open Graph y Twitter Card;
- JSON-LD conservador con `LodgingBusiness`, `Accommodation`, `Offer` y breadcrumbs;
- sitemap dinámico;
- slugs de inventario ASCII deterministas;
- páginas manuales de destino/colección/editorial;
- redirects SEO en el dominio C;
- rebuild programático desde inventario activo.

FVS no emite `VacationRental` específico mientras el dataset no cumpla los requisitos especiales de ese programa (geo, identificadores e imágenes suficientes). Esto evita structured data engañoso.

## Publicidad / campañas

El core posee campañas, creatividades, aprobaciones, jobs, métricas, atribución y límites financieros. Los workers sólo ejecutan jobs ya autorizados.

Canales soportados por el modelo: Meta, Instagram, Google, TikTok, LinkedIn, X, email y webhook. En producción se recomienda apuntar `FVS_MARKETING_<CHANNEL>_URL` a un adapter interno/versionado que use la API oficial correspondiente. Esto evita acoplar el core a cambios frecuentes de cada red.

Guardrails disponibles:

- presupuesto total y diario;
- máximo de gasto diario;
- stop-loss;
- frequency cap 7 días;
- target ROAS;
- audiencia/geografía/placements;
- reglas de optimización;
- experimento/variantes;
- aprobación humana de campaña y creativo;
- lease + fencing por job;
- pausa como gate de ejecución;
- retry/backoff/dead-letter;
- auditoría de cambios de presupuesto.

## Atribución

El frontend registra eventos first-party anónimos con `visitor_id`, `session_id`, UTM y referrer. No envía email ni tarjeta a la capa de atribución. El embudo soporta `landing`, `lead`, `cart`, `checkout`, `booking` y `revenue`.

## Principio de seguridad

La automatización puede aumentar volumen, pero no puede saltarse aprobación, límites de presupuesto, stop-loss o fencing. Cambios de dinero/reserva permanecen fuera de la autonomía del modelo de soporte.
