-- FVS current schema.
-- Single installable schema snapshot for this repository.

-- domain section: core
CREATE TABLE IF NOT EXISTS schema_migrations (
  version VARCHAR(128) PRIMARY KEY,
  checksum CHAR(64) NOT NULL,
  dirty TINYINT(1) NOT NULL DEFAULT 0,
  applied_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS carts (
  id CHAR(36) PRIMARY KEY,
  secret_hash BINARY(32) NOT NULL,
  status ENUM('open','checkout','paid','manual_review','cancelled','expired') NOT NULL DEFAULT 'open',
  version BIGINT UNSIGNED NOT NULL DEFAULT 1,
  customer_email VARCHAR(254) NULL,
  terms_version VARCHAR(64) NULL,
  terms_accepted_at DATETIME(6) NULL,
  created_at DATETIME(6) NOT NULL,
  updated_at DATETIME(6) NOT NULL,
  INDEX ix_carts_status_updated(status, updated_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS inventory_slots (
  id CHAR(36) PRIMARY KEY,
  resort VARCHAR(160) NOT NULL,
  unit_code VARCHAR(80) NOT NULL,
  unit_name VARCHAR(160) NOT NULL,
  city VARCHAR(120) NULL,
  country VARCHAR(120) NULL,
  check_in DATE NOT NULL,
  check_out DATE NOT NULL,
  week_number SMALLINT UNSIGNED NULL,
  booking_mode ENUM('fixed','floating') NOT NULL DEFAULT 'fixed',
  float_group VARCHAR(100) NULL,
  season VARCHAR(40) NULL,
  max_guests SMALLINT UNSIGNED NOT NULL DEFAULT 2,
  price_minor BIGINT UNSIGNED NOT NULL,
  currency CHAR(3) NOT NULL DEFAULT 'MXN',
  image_url VARCHAR(1024) NULL,
  short_description VARCHAR(500) NULL,
  source_ref VARCHAR(191) NULL,
  active TINYINT(1) NOT NULL DEFAULT 1,
  is_booked TINYINT(1) NOT NULL DEFAULT 0,
  held_by_cart_id CHAR(36) NULL,
  hold_expires_at DATETIME(6) NULL,
  created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  CONSTRAINT ck_inventory_dates CHECK (check_out > check_in),
  CONSTRAINT ck_inventory_price CHECK (price_minor > 0),
  CONSTRAINT ck_inventory_guests CHECK (max_guests > 0),
  CONSTRAINT fk_inventory_hold_cart FOREIGN KEY (held_by_cart_id) REFERENCES carts(id) ON DELETE SET NULL,
  UNIQUE KEY uq_inventory_source_ref(source_ref),
  INDEX ix_inventory_search(active,is_booked,check_in,check_out,max_guests),
  INDEX ix_inventory_hold(held_by_cart_id,hold_expires_at),
  INDEX ix_inventory_resort(resort,unit_code,check_in)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS cart_items (
  cart_id CHAR(36) NOT NULL,
  slot_id CHAR(36) NOT NULL,
  guests SMALLINT UNSIGNED NOT NULL,
  price_minor BIGINT UNSIGNED NOT NULL,
  currency CHAR(3) NOT NULL,
  hold_expires_at DATETIME(6) NOT NULL,
  created_at DATETIME(6) NOT NULL,
  updated_at DATETIME(6) NOT NULL,
  PRIMARY KEY(cart_id,slot_id),
  CONSTRAINT fk_cart_items_cart FOREIGN KEY(cart_id) REFERENCES carts(id) ON DELETE CASCADE,
  CONSTRAINT fk_cart_items_slot FOREIGN KEY(slot_id) REFERENCES inventory_slots(id),
  INDEX ix_cart_items_expiry(hold_expires_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS payment_attempts (
  id CHAR(36) PRIMARY KEY,
  cart_id CHAR(36) NOT NULL,
  cart_version BIGINT UNSIGNED NOT NULL,
  provider ENUM('stripe','mercadopago') NOT NULL,
  status ENUM('creating','pending','paid','failed','manual_review') NOT NULL,
  amount_minor BIGINT UNSIGNED NOT NULL,
  currency CHAR(3) NOT NULL,
  email VARCHAR(254) NOT NULL,
  terms_version VARCHAR(64) NOT NULL,
  idempotency_key VARCHAR(128) NOT NULL,
  provider_checkout_id VARCHAR(255) NULL,
  provider_payment_id VARCHAR(255) NULL,
  client_secret VARCHAR(512) NULL,
  redirect_url VARCHAR(2048) NULL,
  last_error VARCHAR(1000) NULL,
  paid_at DATETIME(6) NULL,
  created_at DATETIME(6) NOT NULL,
  updated_at DATETIME(6) NOT NULL,
  CONSTRAINT fk_payment_cart FOREIGN KEY(cart_id) REFERENCES carts(id),
  UNIQUE KEY uq_attempt_cart_version_provider(cart_id,cart_version,provider),
  UNIQUE KEY uq_attempt_idempotency(idempotency_key),
  UNIQUE KEY uq_attempt_provider_payment(provider,provider_payment_id),
  INDEX ix_attempt_cart_status(cart_id,status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS orders (
  id CHAR(36) PRIMARY KEY,
  cart_id CHAR(36) NOT NULL,
  payment_attempt_id CHAR(36) NOT NULL,
  status ENUM('confirmed','cancelled','refunded','manual_review') NOT NULL,
  total_minor BIGINT UNSIGNED NOT NULL,
  currency CHAR(3) NOT NULL,
  customer_email VARCHAR(254) NOT NULL,
  created_at DATETIME(6) NOT NULL,
  updated_at DATETIME(6) NOT NULL,
  UNIQUE KEY uq_orders_cart(cart_id),
  UNIQUE KEY uq_orders_attempt(payment_attempt_id),
  CONSTRAINT fk_orders_cart FOREIGN KEY(cart_id) REFERENCES carts(id),
  CONSTRAINT fk_orders_attempt FOREIGN KEY(payment_attempt_id) REFERENCES payment_attempts(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS order_items (
  id CHAR(36) PRIMARY KEY,
  order_id CHAR(36) NOT NULL,
  slot_id CHAR(36) NOT NULL,
  guests SMALLINT UNSIGNED NOT NULL,
  price_minor BIGINT UNSIGNED NOT NULL,
  currency CHAR(3) NOT NULL,
  resort VARCHAR(160) NOT NULL,
  unit_code VARCHAR(80) NOT NULL,
  unit_name VARCHAR(160) NOT NULL,
  check_in DATE NOT NULL,
  check_out DATE NOT NULL,
  city VARCHAR(120) NULL,
  country VARCHAR(120) NULL,
  created_at DATETIME(6) NOT NULL,
  CONSTRAINT fk_order_items_order FOREIGN KEY(order_id) REFERENCES orders(id),
  CONSTRAINT fk_order_items_slot FOREIGN KEY(slot_id) REFERENCES inventory_slots(id),
  UNIQUE KEY uq_order_slot(order_id,slot_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS webhook_events (
  id CHAR(36) PRIMARY KEY,
  provider ENUM('stripe','mercadopago') NOT NULL,
  event_id VARCHAR(255) NOT NULL,
  lease_owner VARCHAR(128) NULL,
  lease_until DATETIME(6) NULL,
  attempts INT UNSIGNED NOT NULL DEFAULT 0,
  last_error VARCHAR(1000) NULL,
  processed_at DATETIME(6) NULL,
  created_at DATETIME(6) NOT NULL,
  updated_at DATETIME(6) NOT NULL,
  UNIQUE KEY uq_webhook_event(provider,event_id),
  INDEX ix_webhook_lease(processed_at,lease_until)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS outbox_events (
  id CHAR(36) PRIMARY KEY,
  event_type VARCHAR(80) NOT NULL,
  aggregate_id CHAR(36) NOT NULL,
  payload_json JSON NOT NULL,
  status ENUM('pending','processing','retry','done','dead') NOT NULL DEFAULT 'pending',
  attempts INT UNSIGNED NOT NULL DEFAULT 0,
  next_attempt_at DATETIME(6) NOT NULL,
  lease_owner VARCHAR(128) NULL,
  lease_until DATETIME(6) NULL,
  last_error VARCHAR(1000) NULL,
  processed_at DATETIME(6) NULL,
  created_at DATETIME(6) NOT NULL,
  updated_at DATETIME(6) NOT NULL,
  INDEX ix_outbox_claim(status,next_attempt_at,lease_until,created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

-- domain section: admin
CREATE TABLE IF NOT EXISTS admin_imports (
  id CHAR(36) PRIMARY KEY,
  source_name VARCHAR(255) NOT NULL,
  sha256 CHAR(64) NOT NULL,
  rows_seen INT UNSIGNED NOT NULL DEFAULT 0,
  rows_inserted INT UNSIGNED NOT NULL DEFAULT 0,
  rows_updated INT UNSIGNED NOT NULL DEFAULT 0,
  rows_rejected INT UNSIGNED NOT NULL DEFAULT 0,
  status ENUM('running','done','failed') NOT NULL DEFAULT 'running',
  error_text TEXT NULL,
  created_at DATETIME(6) NOT NULL,
  finished_at DATETIME(6) NULL,
  UNIQUE KEY uq_admin_import_hash(sha256)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

-- domain section: outbox_fencing
ALTER TABLE outbox_events
  ADD COLUMN lease_token CHAR(32) NULL AFTER lease_owner;

-- domain section: cart_contract_snapshot
ALTER TABLE cart_items
  ADD COLUMN resort_snapshot VARCHAR(160) NULL AFTER hold_expires_at,
  ADD COLUMN unit_code_snapshot VARCHAR(80) NULL AFTER resort_snapshot,
  ADD COLUMN unit_name_snapshot VARCHAR(160) NULL AFTER unit_code_snapshot,
  ADD COLUMN check_in_snapshot DATE NULL AFTER unit_name_snapshot,
  ADD COLUMN check_out_snapshot DATE NULL AFTER check_in_snapshot,
  ADD COLUMN city_snapshot VARCHAR(120) NULL AFTER check_out_snapshot,
  ADD COLUMN country_snapshot VARCHAR(120) NULL AFTER city_snapshot,
  ADD COLUMN image_url_snapshot VARCHAR(1024) NULL AFTER country_snapshot,
  ADD COLUMN short_description_snapshot VARCHAR(500) NULL AFTER image_url_snapshot,
  ADD COLUMN max_guests_snapshot SMALLINT UNSIGNED NULL AFTER short_description_snapshot;

UPDATE cart_items ci
JOIN inventory_slots s ON s.id=ci.slot_id
SET ci.resort_snapshot=s.resort,
    ci.unit_code_snapshot=s.unit_code,
    ci.unit_name_snapshot=s.unit_name,
    ci.check_in_snapshot=s.check_in,
    ci.check_out_snapshot=s.check_out,
    ci.city_snapshot=s.city,
    ci.country_snapshot=s.country,
    ci.image_url_snapshot=s.image_url,
    ci.short_description_snapshot=s.short_description,
    ci.max_guests_snapshot=s.max_guests;

ALTER TABLE cart_items
  MODIFY resort_snapshot VARCHAR(160) NOT NULL,
  MODIFY unit_code_snapshot VARCHAR(80) NOT NULL,
  MODIFY unit_name_snapshot VARCHAR(160) NOT NULL,
  MODIFY check_in_snapshot DATE NOT NULL,
  MODIFY check_out_snapshot DATE NOT NULL,
  MODIFY max_guests_snapshot SMALLINT UNSIGNED NOT NULL;

-- domain section: webhook_fencing
ALTER TABLE webhook_events
  ADD COLUMN lease_token CHAR(32) NULL AFTER lease_owner;

-- domain section: week_metadata_snapshots
ALTER TABLE cart_items
  ADD COLUMN week_number_snapshot SMALLINT UNSIGNED NULL AFTER max_guests_snapshot,
  ADD COLUMN booking_mode_snapshot ENUM('fixed','floating') NULL AFTER week_number_snapshot,
  ADD COLUMN float_group_snapshot VARCHAR(100) NULL AFTER booking_mode_snapshot;

UPDATE cart_items ci
JOIN inventory_slots s ON s.id=ci.slot_id
SET ci.week_number_snapshot=s.week_number,
    ci.booking_mode_snapshot=s.booking_mode,
    ci.float_group_snapshot=s.float_group;

ALTER TABLE cart_items
  MODIFY booking_mode_snapshot ENUM('fixed','floating') NOT NULL DEFAULT 'fixed';

ALTER TABLE order_items
  ADD COLUMN week_number SMALLINT UNSIGNED NULL AFTER country,
  ADD COLUMN booking_mode ENUM('fixed','floating') NOT NULL DEFAULT 'fixed' AFTER week_number,
  ADD COLUMN float_group VARCHAR(100) NULL AFTER booking_mode;

-- domain section: production_ops
CREATE TABLE IF NOT EXISTS ops_worker_heartbeats (
  worker_name VARCHAR(64) NOT NULL,
  instance_id VARCHAR(128) NOT NULL,
  worker_version VARCHAR(64) NOT NULL,
  started_at DATETIME(6) NOT NULL,
  last_seen_at DATETIME(6) NOT NULL,
  PRIMARY KEY(worker_name,instance_id),
  INDEX ix_worker_last_seen(last_seen_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS ops_actions (
  id CHAR(36) PRIMARY KEY,
  actor VARCHAR(128) NOT NULL,
  action_type VARCHAR(80) NOT NULL,
  target_id VARCHAR(191) NOT NULL,
  created_at DATETIME(6) NOT NULL,
  INDEX ix_ops_actions_created(created_at),
  INDEX ix_ops_actions_target(action_type,target_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

-- domain section: worker_lifecycle
ALTER TABLE ops_worker_heartbeats
  ADD COLUMN stopped_at DATETIME(6) NULL AFTER last_seen_at,
  ADD INDEX ix_worker_active (worker_name, stopped_at, last_seen_at);

-- domain section: support
CREATE TABLE IF NOT EXISTS support_agents (
  id VARCHAR(64) PRIMARY KEY,
  display_name VARCHAR(160) NOT NULL,
  status ENUM('offline','available','busy','away') NOT NULL DEFAULT 'offline',
  max_active SMALLINT UNSIGNED NOT NULL DEFAULT 5,
  active_threads SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  last_seen_at DATETIME(6) NULL,
  created_at DATETIME(6) NOT NULL,
  updated_at DATETIME(6) NOT NULL,
  INDEX ix_support_agents_presence(status,last_seen_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS support_threads (
  id CHAR(36) PRIMARY KEY,
  secret_hash BINARY(32) NOT NULL,
  cart_id CHAR(36) NULL,
  order_id CHAR(36) NULL,
  customer_email VARCHAR(254) NULL,
  subject VARCHAR(240) NOT NULL,
  status ENUM('open','waiting_human','in_progress','waiting_customer','resolved','closed') NOT NULL DEFAULT 'open',
  priority ENUM('low','normal','high','urgent') NOT NULL DEFAULT 'normal',
  channel ENUM('web','email','whatsapp','social','phone') NOT NULL DEFAULT 'web',
  assigned_agent_id VARCHAR(64) NULL,
  ai_enabled TINYINT(1) NOT NULL DEFAULT 1,
  handoff_reason VARCHAR(500) NULL,
  first_response_due_at DATETIME(6) NULL,
  resolution_due_at DATETIME(6) NULL,
  last_message_at DATETIME(6) NOT NULL,
  resolved_at DATETIME(6) NULL,
  created_at DATETIME(6) NOT NULL,
  updated_at DATETIME(6) NOT NULL,
  CONSTRAINT fk_support_thread_agent FOREIGN KEY (assigned_agent_id) REFERENCES support_agents(id) ON DELETE SET NULL,
  INDEX ix_support_queue(status,priority,last_message_at),
  INDEX ix_support_agent(assigned_agent_id,status,last_message_at),
  INDEX ix_support_order(order_id),
  INDEX ix_support_cart(cart_id),
  INDEX ix_support_sla(status,first_response_due_at,resolution_due_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS support_messages (
  id CHAR(36) PRIMARY KEY,
  thread_id CHAR(36) NOT NULL,
  sender_type ENUM('customer','ai','agent','system') NOT NULL,
  sender_id VARCHAR(128) NULL,
  body TEXT NOT NULL,
  visibility ENUM('public','internal') NOT NULL DEFAULT 'public',
  model_name VARCHAR(128) NULL,
  input_tokens INT UNSIGNED NULL,
  output_tokens INT UNSIGNED NULL,
  created_at DATETIME(6) NOT NULL,
  CONSTRAINT fk_support_message_thread FOREIGN KEY (thread_id) REFERENCES support_threads(id) ON DELETE CASCADE,
  INDEX ix_support_messages_thread(thread_id,created_at),
  INDEX ix_support_messages_sender(sender_type,created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS support_events (
  id CHAR(36) PRIMARY KEY,
  thread_id CHAR(36) NOT NULL,
  actor_type ENUM('customer','ai','agent','system','admin') NOT NULL,
  actor_id VARCHAR(128) NULL,
  event_type VARCHAR(80) NOT NULL,
  detail_json JSON NULL,
  created_at DATETIME(6) NOT NULL,
  CONSTRAINT fk_support_event_thread FOREIGN KEY (thread_id) REFERENCES support_threads(id) ON DELETE CASCADE,
  INDEX ix_support_events_thread(thread_id,created_at),
  INDEX ix_support_events_type(event_type,created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

-- domain section: marketing
CREATE TABLE IF NOT EXISTS marketing_campaigns (
  id CHAR(36) PRIMARY KEY,
  name VARCHAR(180) NOT NULL,
  objective ENUM('awareness','traffic','leads','bookings','revenue','retargeting') NOT NULL,
  status ENUM('draft','pending_approval','approved','scheduled','active','paused','completed','cancelled') NOT NULL DEFAULT 'draft',
  automation_mode ENUM('manual','assist','guarded') NOT NULL DEFAULT 'guarded',
  budget_minor BIGINT UNSIGNED NOT NULL DEFAULT 0,
  daily_budget_minor BIGINT UNSIGNED NOT NULL DEFAULT 0,
  max_daily_spend_minor BIGINT UNSIGNED NOT NULL DEFAULT 0,
  currency CHAR(3) NOT NULL DEFAULT 'MXN',
  audience_json JSON NULL,
  start_at DATETIME(6) NULL,
  end_at DATETIME(6) NULL,
  utm_campaign VARCHAR(160) NOT NULL,
  approval_required TINYINT(1) NOT NULL DEFAULT 1,
  created_by VARCHAR(128) NOT NULL,
  approved_by VARCHAR(128) NULL,
  approved_at DATETIME(6) NULL,
  created_at DATETIME(6) NOT NULL,
  updated_at DATETIME(6) NOT NULL,
  INDEX ix_campaign_status(status,start_at,end_at),
  INDEX ix_campaign_objective(objective,status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS marketing_channels (
  id CHAR(36) PRIMARY KEY,
  campaign_id CHAR(36) NOT NULL,
  channel ENUM('meta','instagram','linkedin','tiktok','x','google','email','webhook') NOT NULL,
  provider_account_ref VARCHAR(191) NULL,
  provider_campaign_id VARCHAR(191) NULL,
  status ENUM('draft','ready','active','paused','error','completed') NOT NULL DEFAULT 'draft',
  config_json JSON NULL,
  created_at DATETIME(6) NOT NULL,
  updated_at DATETIME(6) NOT NULL,
  CONSTRAINT fk_marketing_channel_campaign FOREIGN KEY (campaign_id) REFERENCES marketing_campaigns(id) ON DELETE CASCADE,
  UNIQUE KEY uq_marketing_channel(campaign_id,channel,provider_account_ref),
  INDEX ix_marketing_channel_status(channel,status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS marketing_creatives (
  id CHAR(36) PRIMARY KEY,
  campaign_id CHAR(36) NOT NULL,
  channel ENUM('meta','instagram','linkedin','tiktok','x','google','email','webhook') NOT NULL,
  variant_key VARCHAR(64) NOT NULL,
  headline VARCHAR(255) NOT NULL,
  body TEXT NOT NULL,
  cta VARCHAR(80) NULL,
  landing_url VARCHAR(1500) NOT NULL,
  image_url VARCHAR(1500) NULL,
  status ENUM('draft','pending_approval','approved','rejected','archived') NOT NULL DEFAULT 'draft',
  ai_generated TINYINT(1) NOT NULL DEFAULT 0,
  approved_by VARCHAR(128) NULL,
  approved_at DATETIME(6) NULL,
  created_at DATETIME(6) NOT NULL,
  updated_at DATETIME(6) NOT NULL,
  CONSTRAINT fk_marketing_creative_campaign FOREIGN KEY (campaign_id) REFERENCES marketing_campaigns(id) ON DELETE CASCADE,
  UNIQUE KEY uq_marketing_variant(campaign_id,channel,variant_key),
  INDEX ix_marketing_creative_status(campaign_id,status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS marketing_jobs (
  id CHAR(36) PRIMARY KEY,
  campaign_id CHAR(36) NOT NULL,
  creative_id CHAR(36) NULL,
  channel ENUM('meta','instagram','linkedin','tiktok','x','google','email','webhook') NOT NULL,
  action ENUM('publish','pause','resume','sync_metrics','update_budget') NOT NULL,
  payload_json JSON NULL,
  scheduled_at DATETIME(6) NOT NULL,
  status ENUM('pending','processing','retry','done','dead','cancelled') NOT NULL DEFAULT 'pending',
  attempts INT UNSIGNED NOT NULL DEFAULT 0,
  lease_owner VARCHAR(128) NULL,
  lease_token CHAR(32) NULL,
  lease_until DATETIME(6) NULL,
  next_attempt_at DATETIME(6) NOT NULL,
  provider_ref VARCHAR(191) NULL,
  last_error VARCHAR(1000) NULL,
  processed_at DATETIME(6) NULL,
  created_at DATETIME(6) NOT NULL,
  updated_at DATETIME(6) NOT NULL,
  CONSTRAINT fk_marketing_job_campaign FOREIGN KEY (campaign_id) REFERENCES marketing_campaigns(id) ON DELETE CASCADE,
  CONSTRAINT fk_marketing_job_creative FOREIGN KEY (creative_id) REFERENCES marketing_creatives(id) ON DELETE SET NULL,
  INDEX ix_marketing_job_claim(status,next_attempt_at,scheduled_at,lease_until),
  INDEX ix_marketing_job_campaign(campaign_id,status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS marketing_metrics_daily (
  campaign_id CHAR(36) NOT NULL,
  channel ENUM('meta','instagram','linkedin','tiktok','x','google','email','webhook') NOT NULL,
  metric_date DATE NOT NULL,
  impressions BIGINT UNSIGNED NOT NULL DEFAULT 0,
  clicks BIGINT UNSIGNED NOT NULL DEFAULT 0,
  spend_minor BIGINT UNSIGNED NOT NULL DEFAULT 0,
  leads BIGINT UNSIGNED NOT NULL DEFAULT 0,
  bookings BIGINT UNSIGNED NOT NULL DEFAULT 0,
  revenue_minor BIGINT UNSIGNED NOT NULL DEFAULT 0,
  currency CHAR(3) NOT NULL DEFAULT 'MXN',
  updated_at DATETIME(6) NOT NULL,
  PRIMARY KEY(campaign_id,channel,metric_date),
  CONSTRAINT fk_marketing_metrics_campaign FOREIGN KEY (campaign_id) REFERENCES marketing_campaigns(id) ON DELETE CASCADE,
  INDEX ix_marketing_metrics_date(metric_date)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS marketing_attribution_events (
  id CHAR(36) PRIMARY KEY,
  campaign_id CHAR(36) NULL,
  channel VARCHAR(32) NULL,
  creative_id CHAR(36) NULL,
  visitor_id CHAR(36) NULL,
  session_id CHAR(36) NULL,
  order_id CHAR(36) NULL,
  event_type ENUM('impression','click','landing','lead','cart','checkout','booking','revenue') NOT NULL,
  value_minor BIGINT UNSIGNED NOT NULL DEFAULT 0,
  currency CHAR(3) NULL,
  utm_source VARCHAR(120) NULL,
  utm_medium VARCHAR(120) NULL,
  utm_campaign VARCHAR(160) NULL,
  utm_content VARCHAR(160) NULL,
  referrer VARCHAR(1500) NULL,
  created_at DATETIME(6) NOT NULL,
  INDEX ix_marketing_attr_campaign(campaign_id,event_type,created_at),
  INDEX ix_marketing_attr_visitor(visitor_id,created_at),
  INDEX ix_marketing_attr_order(order_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

-- domain section: commerce_telemetry
CREATE TABLE IF NOT EXISTS commerce_events (
  id CHAR(36) PRIMARY KEY,
  event_key VARCHAR(191) NOT NULL,
  event_type VARCHAR(48) NOT NULL,
  source ENUM('server','provider','worker','browser') NOT NULL,
  visitor_id CHAR(36) NULL,
  session_id CHAR(36) NULL,
  cart_id CHAR(36) NULL,
  attempt_id CHAR(36) NULL,
  order_id CHAR(36) NULL,
  slot_id CHAR(36) NULL,
  campaign_id CHAR(36) NULL,
  creative_id CHAR(36) NULL,
  channel VARCHAR(32) NULL,
  provider VARCHAR(32) NULL,
  outcome VARCHAR(48) NULL,
  reason_code VARCHAR(80) NULL,
  value_minor BIGINT UNSIGNED NOT NULL DEFAULT 0,
  currency CHAR(3) NULL,
  query_text VARCHAR(300) NULL,
  result_count INT UNSIGNED NULL,
  metadata_json JSON NULL,
  created_at DATETIME(6) NOT NULL,
  UNIQUE KEY uq_commerce_event_key(event_key),
  INDEX ix_commerce_event_type(event_type,created_at),
  INDEX ix_commerce_event_cart(cart_id,created_at),
  INDEX ix_commerce_event_attempt(attempt_id,created_at),
  INDEX ix_commerce_event_order(order_id,created_at),
  INDEX ix_commerce_event_session(session_id,created_at),
  INDEX ix_commerce_event_campaign(campaign_id,creative_id,created_at),
  INDEX ix_commerce_event_query(event_type,result_count,created_at),
  INDEX ix_commerce_event_currency(currency,event_type,created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

-- domain section: seo
CREATE TABLE IF NOT EXISTS seo_pages (
  id CHAR(36) PRIMARY KEY,
  inventory_slot_id CHAR(36) NULL,
  slug VARCHAR(220) NOT NULL,
  locale VARCHAR(16) NOT NULL DEFAULT 'es-MX',
  page_type ENUM('property','destination','collection','editorial') NOT NULL DEFAULT 'property',
  title VARCHAR(180) NOT NULL,
  meta_description VARCHAR(320) NOT NULL,
  h1 VARCHAR(220) NOT NULL,
  body_text MEDIUMTEXT NULL,
  faq_json JSON NULL,
  canonical_path VARCHAR(500) NOT NULL,
  og_image_url VARCHAR(1500) NULL,
  indexable TINYINT(1) NOT NULL DEFAULT 1,
  priority DECIMAL(3,2) NOT NULL DEFAULT 0.70,
  changefreq ENUM('always','hourly','daily','weekly','monthly','yearly','never') NOT NULL DEFAULT 'daily',
  published_at DATETIME(6) NULL,
  updated_at DATETIME(6) NOT NULL,
  CONSTRAINT fk_seo_inventory FOREIGN KEY (inventory_slot_id) REFERENCES inventory_slots(id) ON DELETE SET NULL,
  UNIQUE KEY uq_seo_slug_locale(slug,locale),
  INDEX ix_seo_sitemap(indexable,published_at,updated_at),
  INDEX ix_seo_inventory(inventory_slot_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE IF NOT EXISTS seo_redirects (
  source_path VARCHAR(500) PRIMARY KEY,
  target_path VARCHAR(500) NOT NULL,
  status_code SMALLINT UNSIGNED NOT NULL DEFAULT 301,
  active TINYINT(1) NOT NULL DEFAULT 1,
  created_at DATETIME(6) NOT NULL,
  updated_at DATETIME(6) NOT NULL,
  INDEX ix_seo_redirect_active(active)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

-- domain section: growth_guardrails
ALTER TABLE marketing_campaigns
  ADD COLUMN frequency_cap_7d SMALLINT UNSIGNED NOT NULL DEFAULT 6 AFTER max_daily_spend_minor,
  ADD COLUMN target_roas_bps INT UNSIGNED NOT NULL DEFAULT 0 AFTER frequency_cap_7d,
  ADD COLUMN stop_loss_minor BIGINT UNSIGNED NOT NULL DEFAULT 0 AFTER target_roas_bps,
  ADD COLUMN geo_json JSON NULL AFTER audience_json,
  ADD COLUMN placements_json JSON NULL AFTER geo_json,
  ADD COLUMN optimization_rules_json JSON NULL AFTER placements_json,
  ADD COLUMN experiment_json JSON NULL AFTER optimization_rules_json;

CREATE TABLE marketing_budget_events (
  id CHAR(36) PRIMARY KEY,
  campaign_id CHAR(36) NOT NULL,
  actor VARCHAR(128) NOT NULL,
  action ENUM('configure','approve','pause','resume','increase','decrease','stop_loss') NOT NULL,
  previous_daily_budget_minor BIGINT UNSIGNED NOT NULL DEFAULT 0,
  new_daily_budget_minor BIGINT UNSIGNED NOT NULL DEFAULT 0,
  detail_json JSON NULL,
  created_at DATETIME(6) NOT NULL,
  CONSTRAINT fk_marketing_budget_campaign FOREIGN KEY (campaign_id) REFERENCES marketing_campaigns(id) ON DELETE CASCADE,
  INDEX ix_marketing_budget_campaign(campaign_id,created_at),
  INDEX ix_marketing_budget_action(action,created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

-- domain section: discovery_search
ALTER TABLE inventory_slots
  ADD COLUMN latitude DECIMAL(9,6) NULL AFTER country,
  ADD COLUMN longitude DECIMAL(9,6) NULL AFTER latitude,
  ADD COLUMN property_type VARCHAR(60) NULL AFTER longitude,
  ADD COLUMN bedrooms TINYINT UNSIGNED NULL AFTER property_type,
  ADD COLUMN bathrooms DECIMAL(4,1) NULL AFTER bedrooms,
  ADD COLUMN rating_x100 SMALLINT UNSIGNED NULL AFTER bathrooms,
  ADD COLUMN amenities_json JSON NULL AFTER rating_x100,
  ADD COLUMN search_text TEXT NULL AFTER amenities_json,
  ADD INDEX ix_inventory_geo(latitude,longitude),
  ADD INDEX ix_inventory_country_city(country,city),
  ADD INDEX ix_inventory_type(property_type),
  ADD FULLTEXT INDEX ft_inventory_search(search_text);

UPDATE inventory_slots
SET search_text=LOWER(CONCAT_WS(' ',resort,unit_code,unit_name,city,country,property_type,season,short_description,JSON_UNQUOTE(amenities_json)));

DROP TRIGGER IF EXISTS trg_inventory_search_bi;
CREATE TRIGGER trg_inventory_search_bi BEFORE INSERT ON inventory_slots FOR EACH ROW
SET NEW.search_text=LOWER(CONCAT_WS(' ',NEW.resort,NEW.unit_code,NEW.unit_name,NEW.city,NEW.country,NEW.property_type,NEW.season,NEW.short_description,JSON_UNQUOTE(NEW.amenities_json)));

DROP TRIGGER IF EXISTS trg_inventory_search_bu;
CREATE TRIGGER trg_inventory_search_bu BEFORE UPDATE ON inventory_slots FOR EACH ROW
SET NEW.search_text=LOWER(CONCAT_WS(' ',NEW.resort,NEW.unit_code,NEW.unit_name,NEW.city,NEW.country,NEW.property_type,NEW.season,NEW.short_description,JSON_UNQUOTE(NEW.amenities_json)));

-- domain section: commerce_experience
ALTER TABLE carts
  ADD COLUMN promo_code VARCHAR(64) NULL AFTER terms_accepted_at,
  ADD COLUMN referral_code VARCHAR(64) NULL AFTER promo_code,
  ADD COLUMN source_channel VARCHAR(32) NULL AFTER referral_code,
  ADD COLUMN source_campaign_id CHAR(36) NULL AFTER source_channel,
  ADD COLUMN source_creative_id CHAR(36) NULL AFTER source_campaign_id,
  ADD COLUMN social_link_token CHAR(32) NULL AFTER source_creative_id;

ALTER TABLE payment_attempts
  ADD COLUMN subtotal_minor BIGINT UNSIGNED NOT NULL DEFAULT 0 AFTER amount_minor,
  ADD COLUMN addons_minor BIGINT UNSIGNED NOT NULL DEFAULT 0 AFTER subtotal_minor,
  ADD COLUMN discount_minor BIGINT UNSIGNED NOT NULL DEFAULT 0 AFTER addons_minor,
  ADD COLUMN promo_code VARCHAR(64) NULL AFTER discount_minor,
  ADD COLUMN source_channel VARCHAR(32) NULL AFTER promo_code;

ALTER TABLE orders
  ADD COLUMN subtotal_minor BIGINT UNSIGNED NOT NULL DEFAULT 0 AFTER total_minor,
  ADD COLUMN addons_minor BIGINT UNSIGNED NOT NULL DEFAULT 0 AFTER subtotal_minor,
  ADD COLUMN discount_minor BIGINT UNSIGNED NOT NULL DEFAULT 0 AFTER addons_minor,
  ADD COLUMN promo_code VARCHAR(64) NULL AFTER discount_minor,
  ADD COLUMN source_channel VARCHAR(32) NULL AFTER promo_code,
  ADD COLUMN source_campaign_id CHAR(36) NULL AFTER source_channel,
  ADD COLUMN source_creative_id CHAR(36) NULL AFTER source_campaign_id;

ALTER TABLE marketing_channels MODIFY channel ENUM('meta','instagram','linkedin','tiktok','x','google','email','webhook','pinterest','whatsapp','youtube') NOT NULL;
ALTER TABLE marketing_creatives MODIFY channel ENUM('meta','instagram','linkedin','tiktok','x','google','email','webhook','pinterest','whatsapp','youtube') NOT NULL;
ALTER TABLE marketing_jobs MODIFY channel ENUM('meta','instagram','linkedin','tiktok','x','google','email','webhook','pinterest','whatsapp','youtube') NOT NULL;
ALTER TABLE marketing_jobs MODIFY action ENUM('publish','pause','resume','sync_metrics','update_budget','sync_catalog','send_conversion','recover_abandonment','publish_collection') NOT NULL;
ALTER TABLE marketing_metrics_daily MODIFY channel ENUM('meta','instagram','linkedin','tiktok','x','google','email','webhook','pinterest','whatsapp','youtube') NOT NULL;

CREATE TABLE commerce_promotions (
  id CHAR(36) PRIMARY KEY,
  code VARCHAR(64) NOT NULL,
  name VARCHAR(180) NOT NULL,
  discount_type ENUM('percent_bps','fixed_minor') NOT NULL,
  discount_value BIGINT UNSIGNED NOT NULL,
  min_subtotal_minor BIGINT UNSIGNED NOT NULL DEFAULT 0,
  max_discount_minor BIGINT UNSIGNED NOT NULL DEFAULT 0,
  currency CHAR(3) NOT NULL DEFAULT 'MXN',
  channel_scope JSON NULL,
  starts_at DATETIME(6) NULL,
  ends_at DATETIME(6) NULL,
  usage_limit BIGINT UNSIGNED NOT NULL DEFAULT 0,
  usage_count BIGINT UNSIGNED NOT NULL DEFAULT 0,
  active TINYINT(1) NOT NULL DEFAULT 1,
  created_by VARCHAR(128) NOT NULL,
  created_at DATETIME(6) NOT NULL,
  updated_at DATETIME(6) NOT NULL,
  UNIQUE KEY uq_commerce_promo_code(code),
  INDEX ix_commerce_promo_active(active,starts_at,ends_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE commerce_addons (
  id CHAR(36) PRIMARY KEY,
  code VARCHAR(64) NOT NULL,
  name VARCHAR(180) NOT NULL,
  description VARCHAR(700) NULL,
  category VARCHAR(64) NOT NULL DEFAULT 'experience',
  pricing_model ENUM('flat','per_guest') NOT NULL DEFAULT 'flat',
  price_minor BIGINT UNSIGNED NOT NULL,
  currency CHAR(3) NOT NULL DEFAULT 'MXN',
  icon VARCHAR(32) NULL,
  metadata_json JSON NULL,
  active TINYINT(1) NOT NULL DEFAULT 1,
  sort_order INT NOT NULL DEFAULT 0,
  created_at DATETIME(6) NOT NULL,
  updated_at DATETIME(6) NOT NULL,
  UNIQUE KEY uq_commerce_addon_code(code),
  INDEX ix_commerce_addon_active(active,sort_order)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE cart_addons (
  cart_id CHAR(36) NOT NULL,
  addon_id CHAR(36) NOT NULL,
  code_snapshot VARCHAR(64) NOT NULL,
  quantity SMALLINT UNSIGNED NOT NULL DEFAULT 1,
  unit_price_minor BIGINT UNSIGNED NOT NULL,
  currency CHAR(3) NOT NULL,
  name_snapshot VARCHAR(180) NOT NULL,
  pricing_model_snapshot ENUM('flat','per_guest') NOT NULL,
  created_at DATETIME(6) NOT NULL,
  updated_at DATETIME(6) NOT NULL,
  PRIMARY KEY(cart_id,addon_id),
  CONSTRAINT fk_cart_addons_cart FOREIGN KEY(cart_id) REFERENCES carts(id) ON DELETE CASCADE,
  CONSTRAINT fk_cart_addons_addon FOREIGN KEY(addon_id) REFERENCES commerce_addons(id),
  INDEX ix_cart_addons_cart(cart_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE order_addons (
  id CHAR(36) PRIMARY KEY,
  order_id CHAR(36) NOT NULL,
  addon_id CHAR(36) NULL,
  code VARCHAR(64) NOT NULL,
  name VARCHAR(180) NOT NULL,
  quantity SMALLINT UNSIGNED NOT NULL,
  unit_price_minor BIGINT UNSIGNED NOT NULL,
  total_minor BIGINT UNSIGNED NOT NULL,
  currency CHAR(3) NOT NULL,
  created_at DATETIME(6) NOT NULL,
  CONSTRAINT fk_order_addons_order FOREIGN KEY(order_id) REFERENCES orders(id) ON DELETE CASCADE,
  CONSTRAINT fk_order_addons_addon FOREIGN KEY(addon_id) REFERENCES commerce_addons(id) ON DELETE SET NULL,
  INDEX ix_order_addons_order(order_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE commerce_membership_plans (
  id CHAR(36) PRIMARY KEY,
  code VARCHAR(48) NOT NULL,
  name VARCHAR(160) NOT NULL,
  description VARCHAR(700) NULL,
  billing_period ENUM('monthly','annual','lifetime') NOT NULL,
  fee_minor BIGINT UNSIGNED NOT NULL,
  currency CHAR(3) NOT NULL DEFAULT 'MXN',
  booking_discount_bps INT UNSIGNED NOT NULL DEFAULT 0,
  points_multiplier_bps INT UNSIGNED NOT NULL DEFAULT 10000,
  benefits_json JSON NULL,
  active TINYINT(1) NOT NULL DEFAULT 1,
  sort_order INT NOT NULL DEFAULT 0,
  created_at DATETIME(6) NOT NULL,
  updated_at DATETIME(6) NOT NULL,
  UNIQUE KEY uq_membership_plan_code(code),
  INDEX ix_membership_plan_active(active,sort_order)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE commerce_memberships (
  id CHAR(36) PRIMARY KEY,
  email_hash BINARY(32) NOT NULL,
  plan_id CHAR(36) NOT NULL,
  status ENUM('active','paused','cancelled','expired') NOT NULL DEFAULT 'active',
  starts_at DATETIME(6) NOT NULL,
  ends_at DATETIME(6) NULL,
  provider_customer_ref VARCHAR(191) NULL,
  provider_subscription_ref VARCHAR(191) NULL,
  created_at DATETIME(6) NOT NULL,
  updated_at DATETIME(6) NOT NULL,
  CONSTRAINT fk_membership_plan FOREIGN KEY(plan_id) REFERENCES commerce_membership_plans(id),
  INDEX ix_membership_email_status(email_hash,status),
  UNIQUE KEY uq_membership_provider(provider_subscription_ref)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE commerce_loyalty_accounts (
  id CHAR(36) PRIMARY KEY,
  email_hash BINARY(32) NOT NULL,
  points_balance BIGINT NOT NULL DEFAULT 0,
  lifetime_points BIGINT UNSIGNED NOT NULL DEFAULT 0,
  tier ENUM('explorer','insider','elite') NOT NULL DEFAULT 'explorer',
  created_at DATETIME(6) NOT NULL,
  updated_at DATETIME(6) NOT NULL,
  UNIQUE KEY uq_loyalty_email(email_hash),
  INDEX ix_loyalty_tier(tier,points_balance)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE commerce_loyalty_rules (
  currency CHAR(3) PRIMARY KEY,
  minor_per_point BIGINT UNSIGNED NOT NULL,
  active TINYINT(1) NOT NULL DEFAULT 1,
  updated_at DATETIME(6) NOT NULL,
  CONSTRAINT ck_loyalty_minor_per_point CHECK (minor_per_point > 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE commerce_loyalty_ledger (
  id CHAR(36) PRIMARY KEY,
  account_id CHAR(36) NOT NULL,
  order_id CHAR(36) NULL,
  event_type ENUM('earn','redeem','expire','adjust','referral') NOT NULL,
  points_delta BIGINT NOT NULL,
  detail_json JSON NULL,
  created_at DATETIME(6) NOT NULL,
  CONSTRAINT fk_loyalty_ledger_account FOREIGN KEY(account_id) REFERENCES commerce_loyalty_accounts(id) ON DELETE CASCADE,
  CONSTRAINT fk_loyalty_ledger_order FOREIGN KEY(order_id) REFERENCES orders(id) ON DELETE SET NULL,
  INDEX ix_loyalty_ledger_account(account_id,created_at),
  UNIQUE KEY uq_loyalty_order_event(account_id,order_id,event_type)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE commerce_referral_codes (
  id CHAR(36) PRIMARY KEY,
  code VARCHAR(64) NOT NULL,
  owner_email_hash BINARY(32) NULL,
  invitee_discount_minor BIGINT UNSIGNED NOT NULL DEFAULT 0,
  advocate_points BIGINT UNSIGNED NOT NULL DEFAULT 0,
  currency CHAR(3) NOT NULL DEFAULT 'MXN',
  usage_limit BIGINT UNSIGNED NOT NULL DEFAULT 0,
  usage_count BIGINT UNSIGNED NOT NULL DEFAULT 0,
  active TINYINT(1) NOT NULL DEFAULT 1,
  starts_at DATETIME(6) NULL,
  ends_at DATETIME(6) NULL,
  created_at DATETIME(6) NOT NULL,
  updated_at DATETIME(6) NOT NULL,
  UNIQUE KEY uq_referral_code(code),
  INDEX ix_referral_active(active,starts_at,ends_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE commerce_referral_events (
  id CHAR(36) PRIMARY KEY,
  referral_code_id CHAR(36) NOT NULL,
  order_id CHAR(36) NULL,
  event_type ENUM('landing','cart','booking','reward') NOT NULL,
  value_minor BIGINT UNSIGNED NOT NULL DEFAULT 0,
  created_at DATETIME(6) NOT NULL,
  CONSTRAINT fk_referral_event_code FOREIGN KEY(referral_code_id) REFERENCES commerce_referral_codes(id),
  CONSTRAINT fk_referral_event_order FOREIGN KEY(order_id) REFERENCES orders(id) ON DELETE SET NULL,
  INDEX ix_referral_events_code(referral_code_id,created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE commerce_collections (
  id CHAR(36) PRIMARY KEY,
  slug VARCHAR(120) NOT NULL,
  name VARCHAR(180) NOT NULL,
  subtitle VARCHAR(300) NULL,
  hero_image_url VARCHAR(1500) NULL,
  badge VARCHAR(64) NULL,
  filter_json JSON NULL,
  merchandising_json JSON NULL,
  active TINYINT(1) NOT NULL DEFAULT 1,
  sort_order INT NOT NULL DEFAULT 0,
  created_by VARCHAR(128) NOT NULL,
  created_at DATETIME(6) NOT NULL,
  updated_at DATETIME(6) NOT NULL,
  UNIQUE KEY uq_collection_slug(slug),
  INDEX ix_collection_active(active,sort_order)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE commerce_collection_items (
  collection_id CHAR(36) NOT NULL,
  slot_id CHAR(36) NOT NULL,
  rank_score INT NOT NULL DEFAULT 0,
  badge VARCHAR(64) NULL,
  note VARCHAR(240) NULL,
  created_at DATETIME(6) NOT NULL,
  PRIMARY KEY(collection_id,slot_id),
  CONSTRAINT fk_collection_item_collection FOREIGN KEY(collection_id) REFERENCES commerce_collections(id) ON DELETE CASCADE,
  CONSTRAINT fk_collection_item_slot FOREIGN KEY(slot_id) REFERENCES inventory_slots(id) ON DELETE CASCADE,
  INDEX ix_collection_item_rank(collection_id,rank_score)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE social_sales_links (
  id CHAR(36) PRIMARY KEY,
  token CHAR(32) NOT NULL,
  channel ENUM('meta','instagram','tiktok','pinterest','whatsapp','youtube','x','other') NOT NULL,
  slot_id CHAR(36) NULL,
  collection_id CHAR(36) NULL,
  campaign_id CHAR(36) NULL,
  creative_id CHAR(36) NULL,
  promo_code VARCHAR(64) NULL,
  utm_source VARCHAR(120) NOT NULL,
  utm_medium VARCHAR(120) NOT NULL DEFAULT 'social',
  utm_campaign VARCHAR(160) NULL,
  utm_content VARCHAR(160) NULL,
  clicks BIGINT UNSIGNED NOT NULL DEFAULT 0,
  carts BIGINT UNSIGNED NOT NULL DEFAULT 0,
  bookings BIGINT UNSIGNED NOT NULL DEFAULT 0,
  active TINYINT(1) NOT NULL DEFAULT 1,
  expires_at DATETIME(6) NULL,
  created_by VARCHAR(128) NOT NULL,
  created_at DATETIME(6) NOT NULL,
  updated_at DATETIME(6) NOT NULL,
  UNIQUE KEY uq_social_sales_token(token),
  CONSTRAINT fk_social_link_slot FOREIGN KEY(slot_id) REFERENCES inventory_slots(id) ON DELETE SET NULL,
  CONSTRAINT fk_social_link_collection FOREIGN KEY(collection_id) REFERENCES commerce_collections(id) ON DELETE SET NULL,
  CONSTRAINT fk_social_link_campaign FOREIGN KEY(campaign_id) REFERENCES marketing_campaigns(id) ON DELETE SET NULL,
  CONSTRAINT fk_social_link_creative FOREIGN KEY(creative_id) REFERENCES marketing_creatives(id) ON DELETE SET NULL,
  INDEX ix_social_sales_channel(channel,active,created_at),
  INDEX ix_social_sales_campaign(campaign_id,creative_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE social_sales_link_revenue (
  link_id CHAR(36) NOT NULL,
  currency CHAR(3) NOT NULL,
  bookings BIGINT UNSIGNED NOT NULL DEFAULT 0,
  revenue_minor BIGINT UNSIGNED NOT NULL DEFAULT 0,
  updated_at DATETIME(6) NOT NULL,
  PRIMARY KEY(link_id,currency),
  CONSTRAINT fk_social_link_revenue_link FOREIGN KEY(link_id) REFERENCES social_sales_links(id) ON DELETE CASCADE,
  INDEX ix_social_link_revenue_currency(currency,updated_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE social_catalog_items (
  channel ENUM('meta','instagram','tiktok','pinterest','whatsapp','youtube') NOT NULL,
  slot_id CHAR(36) NOT NULL,
  provider_product_id VARCHAR(191) NULL,
  content_hash CHAR(64) NULL,
  status ENUM('pending','synced','error','disabled') NOT NULL DEFAULT 'pending',
  last_error VARCHAR(1000) NULL,
  synced_at DATETIME(6) NULL,
  updated_at DATETIME(6) NOT NULL,
  PRIMARY KEY(channel,slot_id),
  CONSTRAINT fk_social_catalog_slot FOREIGN KEY(slot_id) REFERENCES inventory_slots(id) ON DELETE CASCADE,
  INDEX ix_social_catalog_status(channel,status,updated_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE social_leads (
  id CHAR(36) PRIMARY KEY,
  channel ENUM('meta','instagram','tiktok','pinterest','whatsapp','youtube','x','other') NOT NULL,
  provider_lead_id VARCHAR(191) NOT NULL,
  campaign_id CHAR(36) NULL,
  creative_id CHAR(36) NULL,
  support_thread_id CHAR(36) NULL,
  contact_json JSON NULL,
  message_text VARCHAR(4000) NULL,
  status ENUM('new','qualified','in_support','converted','closed') NOT NULL DEFAULT 'new',
  created_at DATETIME(6) NOT NULL,
  updated_at DATETIME(6) NOT NULL,
  UNIQUE KEY uq_social_lead_provider(channel,provider_lead_id),
  CONSTRAINT fk_social_lead_campaign FOREIGN KEY(campaign_id) REFERENCES marketing_campaigns(id) ON DELETE SET NULL,
  CONSTRAINT fk_social_lead_creative FOREIGN KEY(creative_id) REFERENCES marketing_creatives(id) ON DELETE SET NULL,
  CONSTRAINT fk_social_lead_support FOREIGN KEY(support_thread_id) REFERENCES support_threads(id) ON DELETE SET NULL,
  INDEX ix_social_lead_status(channel,status,created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE order_promotions (
  id CHAR(36) PRIMARY KEY,
  order_id CHAR(36) NOT NULL,
  promo_code VARCHAR(64) NOT NULL,
  discount_minor BIGINT UNSIGNED NOT NULL,
  currency CHAR(3) NOT NULL,
  created_at DATETIME(6) NOT NULL,
  CONSTRAINT fk_order_promotion_order FOREIGN KEY(order_id) REFERENCES orders(id) ON DELETE CASCADE,
  INDEX ix_order_promotions_order(order_id),
  INDEX ix_order_promotions_code(promo_code,created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;


INSERT INTO commerce_loyalty_rules(currency,minor_per_point,active,updated_at) VALUES
('MXN',10000,1,UTC_TIMESTAMP())
ON DUPLICATE KEY UPDATE minor_per_point=VALUES(minor_per_point),active=VALUES(active),updated_at=UTC_TIMESTAMP();

-- Starter merchandising primitives are intentionally generic and editable from admin.
INSERT INTO commerce_membership_plans(id,code,name,description,billing_period,fee_minor,currency,booking_discount_bps,points_multiplier_bps,benefits_json,active,sort_order,created_at,updated_at)
VALUES
(UUID(),'INSIDER','FVS Insider','Beneficios de membresía, prioridad y recompensas.','annual',199900,'MXN',300,12500,JSON_ARRAY('priority_support','member_rates','early_access'),1,10,UTC_TIMESTAMP(),UTC_TIMESTAMP()),
(UUID(),'ELITE','FVS Elite','Nivel premium con mayor acumulación y beneficios.','annual',499900,'MXN',500,17500,JSON_ARRAY('priority_support','member_rates','early_access','concierge'),1,20,UTC_TIMESTAMP(),UTC_TIMESTAMP());

INSERT INTO commerce_addons(id,code,name,description,category,pricing_model,price_minor,currency,icon,metadata_json,active,sort_order,created_at,updated_at)
VALUES
(UUID(),'LATE_CHECKOUT','Late checkout','Extiende tu salida cuando la propiedad lo permita.','stay','flat',85000,'MXN','◷',JSON_OBJECT('requires_confirmation',true),1,10,UTC_TIMESTAMP(),UTC_TIMESTAMP()),
(UUID(),'CONCIERGE','Concierge premium','Asistencia prioritaria para coordinar experiencias y solicitudes.','service','flat',120000,'MXN','✦',JSON_OBJECT('service_level','premium'),1,20,UTC_TIMESTAMP(),UTC_TIMESTAMP()),
(UUID(),'AIRPORT_TRANSFER','Traslado aeropuerto','Solicitud de traslado coordinado para la estancia.','transport','flat',145000,'MXN','↗',JSON_OBJECT('requires_details',true),1,30,UTC_TIMESTAMP(),UTC_TIMESTAMP());

INSERT INTO commerce_collections(id,slug,name,subtitle,hero_image_url,badge,filter_json,merchandising_json,active,sort_order,created_by,created_at,updated_at)
VALUES
(UUID(),'playa-escapes','Escapadas de playa','Propiedades para abrir el viaje con mar, sol y cero fricción.',NULL,'Playa',JSON_OBJECT('amenities',JSON_ARRAY('beach')),JSON_OBJECT('tone','sun','layout','immersive'),1,10,'migration',UTC_TIMESTAMP(),UTC_TIMESTAMP()),
(UUID(),'family-mode','Modo familia','Más espacio, más huéspedes y decisiones fáciles para grupos.',NULL,'Familias',JSON_OBJECT('min_bedrooms',2,'guests',4),JSON_OBJECT('tone','family','layout','cards'),1,20,'migration',UTC_TIMESTAMP(),UTC_TIMESTAMP()),
(UUID(),'top-rated','Favoritas de huéspedes','Estancias con mejor rating para decidir más rápido.',NULL,'Top',JSON_OBJECT('min_rating_x100',450,'sort','rating'),JSON_OBJECT('tone','premium','layout','editorial'),1,30,'migration',UTC_TIMESTAMP(),UTC_TIMESTAMP());

