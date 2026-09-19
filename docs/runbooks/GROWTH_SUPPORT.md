# Runbook — soporte IA/humano y Growth

## Antes de habilitar soporte IA

1. Revisar `docs/SUPPORT_KB.md` con Reservas/Legal.
2. Configurar `OPENAI_API_KEY` vía secret manager.
3. Fijar `FVS_SUPPORT_AI_MODEL`; default recomendado del release: `gpt-5.6-luna`.
4. Verificar que worker/outbox esté healthy y con heartbeat.
5. Probar un caso informativo y un caso sensible que fuerce handoff.
6. Confirmar que el agente ve historial completo y puede claim/resolver.

## Antes de habilitar campañas

1. Crear adapters HTTPS por canal y tokens de menor privilegio.
2. Configurar `FVS_MARKETING_SIGNING_SECRET` y allowlist de egress.
3. Crear campaña en `draft`.
4. Definir presupuesto, max daily, stop-loss, frequency cap y target ROAS.
5. Aprobar campaña y creativo con un operador diferente cuando el proceso interno lo requiera.
6. Programar primero `sync_metrics` y una publicación de baja exposición.
7. Verificar atribución y normalización de métricas antes de escalar gasto.

## Kill switch

- Pausar la campaña desde backoffice; jobs de publish/resume/update quedan bloqueados por el gate de campaña.
- Revocar token del adapter si se sospecha compromiso.
- Detener el worker `marketing` si el problema afecta varios canales.
- Conservar jobs/dead-letter para análisis; no borrar filas manualmente.

## Incidente de soporte IA

- Deshabilitar `FVS_SUPPORT_AI_ENABLED` y reiniciar worker.
- Los hilos existentes permanecen en MySQL y pueden ser atendidos por humanos.
- Para contenido incorrecto, preservar thread/event IDs y revisar KB/prompt/modelo antes de reactivar.

## Indicadores a vigilar

- hilos `waiting_human` y SLA de primera respuesta;
- outbox retry/dead;
- marketing jobs retry/dead;
- gasto diario vs `max_daily_spend_minor`;
- ROAS vs target;
- tasa de handoff IA→humano;
- atribución booking/revenue por campaña.
