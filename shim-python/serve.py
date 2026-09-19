from __future__ import annotations
import os
from concurrent.futures import ThreadPoolExecutor
from socketserver import ThreadingMixIn
from threading import BoundedSemaphore
from wsgiref.simple_server import WSGIServer, make_server
from app import application

class PooledWSGIServer(ThreadingMixIn, WSGIServer):
    daemon_threads=False
    allow_reuse_address=True
    def __init__(self,*args,**kwargs):
        self._max_workers=max(2,min(128,int(os.getenv("FVS_PYTHON_WORKERS","16"))))
        self._request_timeout=max(2,min(120,int(os.getenv("FVS_PYTHON_REQUEST_TIMEOUT","15"))))
        self._slots=BoundedSemaphore(self._max_workers*2)
        self._executor=ThreadPoolExecutor(max_workers=self._max_workers,thread_name_prefix="fvs-http")
        super().__init__(*args,**kwargs)
    def get_request(self):
        request,address=super().get_request();request.settimeout(self._request_timeout);return request,address
    def _process_bounded(self,request,client_address):
        try:self.process_request_thread(request,client_address)
        finally:self._slots.release()
    def process_request(self,request,client_address):
        if not self._slots.acquire(blocking=False):
            self.shutdown_request(request);return
        try:self._executor.submit(self._process_bounded,request,client_address)
        except Exception:
            self._slots.release();self.shutdown_request(request);raise
    def server_close(self):
        try: super().server_close()
        finally: self._executor.shutdown(wait=True,cancel_futures=False)

if __name__=="__main__":
    host=os.getenv("FVS_BIND","0.0.0.0");port=int(os.getenv("FVS_PORT","8000"))
    with make_server(host,port,application,server_class=PooledWSGIServer) as httpd:
        print(f"FVS Python shim on http://{host}:{port} workers={httpd._max_workers}",flush=True)
        httpd.serve_forever()
