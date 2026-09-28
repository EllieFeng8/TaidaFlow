@rem TaidaFlow site settings (w2-057). Copy this file to taidaflow-site.bat in the same folder
@rem and edit the values; start-taidaflow.bat and stop-taidaflow.bat read it.
@rem taidaflow-site.bat is not part of the package, so copying a new version over the folder
@rem never overwrites it. No spaces around "=", no trailing backslash.
@rem Data folder (working directory of the app: settings, database, exports, logs).
set "DATADIR=C:\TaidaFlowData"
@rem 1 = also start nginx (web page on http://<IP>/ with NGINX_PORT 80), 0 = app only (web page on :8124).
set "USE_NGINX=1"
set "NGINX_PORT=80"
