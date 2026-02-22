# Security Policy

## Do not commit secrets
Detta repo ska aldrig innehålla:
- WiFi SSID/lösenord
- MQTT credentials/tokens
- Home Assistant long-lived tokens
- certifikat/privata nycklar

Använd `*.example.*` som mallar och lägg riktiga värden i filer som ignoreras av Git.

## Reporting
Rapportera säkerhetsproblem via Issue utan att posta hemligheter.
