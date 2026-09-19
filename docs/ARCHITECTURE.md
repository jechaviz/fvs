# Arquitectura FVS

## Principio rector

Las reglas que afectan dinero, inventario, holds, checkout y reservas viven en **un solo core C**. Los shims PHP/Python no acceden a tablas de negocio: convierten HTTP/JSON, gestionan cookies/origen y hablan con proveedores de pago; después entregan al core datos ya verificados.

```text
Browser
  Vue 3 CDN + SFC runtime + UnoCSS runtime
             |
         Nginx/CSP
             |
      +------+------+
      |             |
 PHP-FPM/FFI   Python/ctypes
      |             |
      +------v------+
          libfvs_core (C17)
               |
        MySQL 8.4 / InnoDB
               |
      transactional outbox
               |
       PHP/Python worker
        PDF + email/SMTP
```

## Modelo de inventario

`inventory_slots` representa una estancia vendible concreta. El slot contiene fechas, semana (`week_number`), modalidad fija/flotante, grupo flotante, unidad, precio, capacidad y estado de hold. Los artículos de carrito (`cart_items`) fotografían los datos comerciales/visuales para que cambios posteriores del inventario no reescriban la experiencia o el voucher histórico.

### Reserva / hold

`fvs_cart_add`:

1. Bloquea el carrito.
2. Exige `status=open`.
3. Bloquea el `inventory_slot` con `FOR UPDATE`.
4. Verifica no reservado, activo, capacidad suficiente y ausencia de hold vivo ajeno.
5. Toma/renueva el hold y guarda snapshot del artículo.
6. Prohíbe moneda mixta dentro del mismo carrito.
7. Incrementa `cart.version`.

## Checkout

`fvs_checkout_begin` toma locks en orden `cart -> slots`, revalida todos los artículos, renueva holds y crea un `payment_attempt` con:

- `cart_version` inmutable para ese intento;
- total entero exacto;
- moneda;
- email snapshot;
- versión de términos;
- idempotency key `fvs:<attempt_id>:v<cart_version>`.

Una recarga no crea un cobro nuevo: el shim recupera el mismo intento y el proveedor recibe la misma idempotency key.

## Conciliación de pago

El webhook verifica la firma y vuelve a consultar el objeto al proveedor. El core no confía sólo en el payload entrante.

`fvs_payment_confirm` toma locks `cart -> payment_attempt -> inventory`, y compara:

- estado/version del carrito;
- estado/version/proveedor del intento;
- ID remoto previamente ligado, si existe;
- importe y moneda reportados por el proveedor;
- número de artículos, suma exacta y moneda actual del snapshot de carrito;
- inventario `active`, `is_booked`, capacidad y holds ajenos.

Si todo es consistente, `orders`, `order_items`, inventario, intento, carrito y evento de outbox se confirman en **una transacción**. Si el pago fue exitoso pero el inventario no puede confirmarse de forma total, se registra `manual_review`.

## Webhooks

`webhook_events` combina idempotencia y lease:

- `ACQUIRED`: este request procesa.
- `BUSY`: otro request tiene lease vivo; el shim responde 503/Retry-After para conservar el reintento del proveedor.
- `DONE`: delivery duplicado ya procesado; responde 200.

Un fallo libera el lease sin marcar `processed_at`; un reintento retoma el evento.

## Outbox y workers

La transacción de reserva inserta `order.confirmed` en `outbox_events`. El worker usa `FOR UPDATE SKIP LOCKED`, lease + token de fencing y máximo de intentos.

Un evento `processing` con lease vencido puede recuperarse. Cada claim produce un token nuevo; un worker antiguo no puede hacer ACK/NACK con un token ya reemplazado.

La generación de PDF y el SMTP se ejecutan **fuera** de la transacción MySQL. El voucher usa snapshots de `order_items`.

## Consistencia temporal

MySQL trabaja en UTC; el core fija la sesión a UTC. `DATETIME(6)` se serializa con sufijo `Z` en API. El frontend usa UTC para fechas de estancia y fecha absoluta para los timers de hold.

## Elección de aislamiento

El despliegue fija `READ COMMITTED`. La exclusión requerida no depende de lecturas implícitas del aislamiento sino de locks explícitos `FOR UPDATE`, lo que reduce rangos bloqueados innecesarios y mantiene el orden de locks controlable.

## Plano operativo

El plano de negocio sigue en C; la capa productiva añade un plano operacional separado:

- `schema_migrations`: checksum/dirty + readiness exacta.
- advisory lock `fvs_schema_migrate`: un solo migrador activo.
- `ops_worker_heartbeats`: lifecycle/heartbeat de workers sin inferir salud sólo desde contenedores.
- `ops_actions`: auditoría de acciones manuales como requeue de dead-letter.
- `/admin/ops` y `/admin/metrics`: observabilidad sin lecturas SQL ad-hoc desde dashboards.

Los shims no adquieren privilegios DDL. El migrator usa una identidad distinta y el backup debe usar una tercera identidad de sólo lectura. Esta separación evita que comprometer un proceso HTTP conceda capacidad automática para alterar el schema.
