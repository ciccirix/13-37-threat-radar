/**
 * 13:37 — free install / onboarding receiver (Google Apps Script + Google Sheet).
 *
 * SETUP (one time, free):
 *   1. Create a new Google Sheet (this is your database + it gives you the count).
 *   2. Extensions -> Apps Script. Delete the sample, paste this whole file. Save.
 *   3. Deploy -> New deployment -> type "Web app".
 *        Execute as:  Me
 *        Who has access:  Anyone
 *      Deploy, authorize, and COPY the "/exec" Web app URL.
 *   4. Paste that URL into ONBOARD_ENDPOINT in src/onboarding.cpp, rebuild, flash.
 *
 * The watch POSTs JSON: {install_id, email, fw, event}. This appends one row per
 * UNIQUE install_id (so re-sends never double-count) and fills the email if the
 * user adds one later. Open the Sheet to see installs (row count) + the email
 * list. Opening the /exec URL in a browser returns the install count as JSON.
 *
 * Privacy: store only what you need, keep the Sheet private, and honor deletion
 * requests (just delete the row). Under GDPR the email is personal data.
 */

const SHEET_NAME = "installs";

function doPost(e) {
  try {
    const data = JSON.parse(e.postData.contents);
    const ss = SpreadsheetApp.getActiveSpreadsheet();
    let sh = ss.getSheetByName(SHEET_NAME) || ss.insertSheet(SHEET_NAME);
    if (sh.getLastRow() === 0) {
      sh.appendRow(["timestamp", "install_id", "email", "fw", "event"]);
    }
    const nRows = Math.max(sh.getLastRow() - 1, 0);
    const ids = nRows ? sh.getRange(2, 2, nRows, 1).getValues().flat() : [];
    const at = data.install_id ? ids.indexOf(data.install_id) : -1;

    if (data.install_id && at === -1) {
      sh.appendRow([new Date(), data.install_id, data.email || "", data.fw || "", data.event || "install"]);
    } else if (at !== -1 && data.email && !sh.getRange(at + 2, 3).getValue()) {
      sh.getRange(at + 2, 3).setValue(data.email);   // fill email added later
    }
    return json({ ok: true, installs: Math.max(sh.getLastRow() - 1, 0) });
  } catch (err) {
    return json({ ok: false, err: String(err) });
  }
}

function doGet() {
  const sh = SpreadsheetApp.getActiveSpreadsheet().getSheetByName(SHEET_NAME);
  return json({ installs: sh ? Math.max(sh.getLastRow() - 1, 0) : 0 });
}

function json(obj) {
  return ContentService.createTextOutput(JSON.stringify(obj))
                       .setMimeType(ContentService.MimeType.JSON);
}
