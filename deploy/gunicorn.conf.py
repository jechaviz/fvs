import os
bind = os.getenv("FVS_BIND", "0.0.0.0") + ":" + os.getenv("FVS_PORT", "8000")
workers = max(2, min(16, int(os.getenv("FVS_GUNICORN_WORKERS", "4"))))
threads = max(2, min(32, int(os.getenv("FVS_GUNICORN_THREADS", "8"))))
worker_class = "gthread"
timeout = max(5, min(120, int(os.getenv("FVS_PYTHON_REQUEST_TIMEOUT", "30"))))
graceful_timeout = max(5, min(120, int(os.getenv("FVS_GRACEFUL_TIMEOUT", "30"))))
keepalive = max(1, min(30, int(os.getenv("FVS_KEEPALIVE", "5"))))
max_requests = max(100, int(os.getenv("FVS_MAX_REQUESTS", "5000")))
max_requests_jitter = max(0, int(os.getenv("FVS_MAX_REQUESTS_JITTER", "500")))
accesslog = "-"
errorlog = "-"
loglevel = os.getenv("FVS_LOG_LEVEL", "info").lower()
capture_output = True
preload_app = False
