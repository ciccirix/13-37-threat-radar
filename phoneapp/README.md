# Phone Link — companion app per iPhone

Dashboard Web Bluetooth che mostra il **Threat Radar** del T-Watch Ultra sul
telefono: banner "ti seguono?", lista dei contatti in co-movimento, allerta
sonora quando un contatto passa a PROBABILE, pulsante per azzerare la memoria
contatti. Tutto in un solo file: `index.html`.

## Uso su iPhone

Safari non supporta Web Bluetooth, serve **Bluefy** (gratis su App Store):

1. Sul watch: THREAT RADAR → tasto **PHONE** (diventa blu = in advertising).
2. Su iPhone: apri `index.html` in Bluefy e tocca **CONNETTI**.
3. Scegli "13-37" nell'elenco dispositivi. Il tasto PHONE sul watch diventa
   verde quando il telefono è connesso.

Per aprire la pagina in Bluefy hai due strade:

- **Hostata**: metti `index.html` su un qualunque hosting statico HTTPS
  (GitHub Pages del fork, Netlify Drop, ecc.) e apri l'URL in Bluefy.
- **Locale**: salva `index.html` nell'app File di iOS e aprilo da Bluefy
  (pulsante apri-file), senza alcun server.

Limiti della strada web: l'app deve restare aperta (niente background BLE,
niente notifiche push iOS). Il wake lock tiene lo schermo acceso finché la
pagina è in primo piano.

## Protocollo BLE

Definito in `src/phone_link.h` (il firmware è la fonte di verità):
servizio `13370000-1337-4001-8000-746872616461` con STATUS (read+notify,
8 byte), THREATS (read, `[ver, count]` + record da 30 byte) e CMD (write,
`0x01` = azzera la memoria contatti). Il telefono non fa polling: rilegge
THREATS solo quando `threats_seq` cambia nello STATUS.
