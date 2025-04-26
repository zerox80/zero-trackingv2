// background/background.js

import { updateRules } from '../js/rule_parser.js';
import createFilterParserModule from '../wasm/filter_parser.js';

// === Konstanten ===
const LOG_PREFIX = "[PagyBlocker]";
const FILTER_LIST_URL = 'filter_lists/filter.txt';
const BADGE_ERROR_COLOR = '#FF0000';
const BADGE_TEXT_INIT_ERROR = 'INIT'; // Kürzer für Badge
const BADGE_TEXT_WASM_ERROR = 'WASM';
const BADGE_TEXT_FETCH_ERROR = 'FETCH';
const BADGE_TEXT_PARSE_ERROR = 'PARSE';
const BADGE_TEXT_RULES_ERROR = 'RULES';
const BADGE_TEXT_EMPTY_LIST = 'EMPTY';

// === Globale Zustandsvariablen ===
let wasmInitPromise = null;
let isInitializing = false; // Lock, um parallele Initialisierungen zu verhindern

// === Hilfsfunktionen ===

/**
 * Stellt sicher, dass das WASM-Modul geladen und initialisiert ist.
 * Verwendet eine Singleton-Promise, um mehrfache Initialisierungen zu vermeiden.
 * Setzt die Promise bei einem Fehler zurück, um einen erneuten Versuch zu ermöglichen.
 */
function ensureWasmModuleLoaded() {
  if (!wasmInitPromise) {
    console.log(`${LOG_PREFIX} Initializing WASM module instance...`);
    console.time(`${LOG_PREFIX} WASM Module Init`);

    wasmInitPromise = createFilterParserModule()
      .then(module => {
        console.timeEnd(`${LOG_PREFIX} WASM Module Init`);
        console.log(`${LOG_PREFIX} WASM module instance initialized.`);
        if (typeof module.parseFilterListWasm !== 'function') {
          // Dieser Fehler sollte idealerweise schon im createFilterParserModule behandelt werden
          throw new Error("WASM module loaded, but 'parseFilterListWasm' function not found.");
        }
        return module;
      })
      .catch(error => {
        console.error(`${LOG_PREFIX} Failed to load or initialize WASM module:`, error);
        if (error.stack) console.error(error.stack);
        // Rücksetzen, damit ein späterer Aufruf neu versucht
        wasmInitPromise = null;
        // Fehler weiterwerfen, damit er in initialize() gefangen wird
        throw new Error(`WASM Init failed: ${error.message}`);
      });
  } else {
    console.log(`${LOG_PREFIX} Using existing WASM module promise.`);
  }
  return wasmInitPromise;
}

/**
 * Löscht den Text und die Hintergrundfarbe des Browser-Action-Badges.
 */
async function clearBadge() {
  try {
    // Prüfen, ob chrome.action und die Methoden existieren
    if (chrome.action?.setBadgeText && chrome.action?.setBadgeBackgroundColor) {
      await chrome.action.setBadgeText({ text: '' });
      // Setze Hintergrundfarbe auf Standard zurück (optional, falls sie immer rot war)
      // await chrome.action.setBadgeBackgroundColor({ color: '#00000000' }); // Transparent oder Standardfarbe
    } else {
        console.warn(`${LOG_PREFIX} chrome.action API not fully available for badge manipulation.`);
    }
  } catch (e) {
    // Fehler beim Setzen des Badges ignorieren, aber loggen könnte hilfreich sein
    console.warn(`${LOG_PREFIX} Could not clear badge:`, e.message);
  }
}

/**
 * Setzt einen Fehlertext und eine rote Hintergrundfarbe für das Browser-Action-Badge.
 * @param {string} text - Der Text, der im Badge angezeigt werden soll (kurz halten!).
 */
async function setErrorBadge(text = BADGE_TEXT_INIT_ERROR) {
  try {
    if (chrome.action?.setBadgeText && chrome.action?.setBadgeBackgroundColor) {
      await chrome.action.setBadgeText({ text });
      await chrome.action.setBadgeBackgroundColor({ color: BADGE_ERROR_COLOR });
    } else {
        console.warn(`${LOG_PREFIX} chrome.action API not fully available for badge manipulation.`);
    }
  } catch (e) {
    console.warn(`${LOG_PREFIX} Could not set error badge:`, e.message);
  }
}

/**
 * Ruft die Filterliste von der angegebenen URL ab.
 * @returns {Promise<string>} Der Inhalt der Filterliste als Text.
 */
async function fetchFilterList() {
  const url = chrome.runtime.getURL(FILTER_LIST_URL);
  console.log(`${LOG_PREFIX} Fetching filter list from ${url}`);
  try {
    const resp = await fetch(url);
    if (!resp.ok) {
      // Spezifischer Fehler für Fetch-Probleme
      throw new Error(`Fetch failed with status: ${resp.status} ${resp.statusText}`);
    }
    const text = await resp.text();
    console.log(`${LOG_PREFIX} Fetched filter list (${text.length} chars).`);
    return text;
  } catch (error) {
      // Fehler weiterwerfen, um ihn in initialize() zu fangen
      console.error(`${LOG_PREFIX} Error during fetch:`, error);
      throw new Error(`Fetch Error: ${error.message}`);
  }
}

/**
 * Parst den Filterlistentext mit dem WASM-Modul.
 * @param {object} module - Das initialisierte WASM-Modul.
 * @param {string} filterListText - Der Text der Filterliste.
 * @returns {Array} Ein Array von Regelobjekten.
 * @throws {Error} Wenn das Parsen fehlschlägt oder die Struktur ungültig ist.
 */
function parseListWithWasm(module, filterListText) {
  console.log(`${LOG_PREFIX} Starting WASM parsing...`);
  console.time(`${LOG_PREFIX} WASM Parsing`);
  let jsonString;
  try {
      jsonString = module.parseFilterListWasm(filterListText);
  } catch (wasmError) {
      console.error(`${LOG_PREFIX} Error calling WASM function:`, wasmError);
      throw new Error(`WASM Execution Error: ${wasmError.message}`);
  } finally {
      console.timeEnd(`${LOG_PREFIX} WASM Parsing`);
  }

  if (!jsonString) {
    // Kann passieren, wenn die Liste leer ist oder nur Kommentare enthält
    console.warn(`${LOG_PREFIX} WASM parser returned empty or null string. Assuming empty rule set.`);
    // Wir geben ein leeres Regelset zurück, anstatt einen Fehler zu werfen
    // Die Behandlung erfolgt dann in initialize()
    return { rules: [], stats: { totalLines: 0, processedRules: 0, skippedLines: 0 } };
  }

  let result;
  try {
    result = JSON.parse(jsonString);
  } catch (e) {
    console.error(`${LOG_PREFIX} Invalid JSON received from WASM:`, jsonString);
    throw new Error(`JSON Parse Error: ${e.message}`);
  }

  // Strukturprüfung
  if (!result || typeof result !== 'object' || !Array.isArray(result.rules) || typeof result.stats !== 'object') {
    console.error(`${LOG_PREFIX} Unexpected structure received from WASM:`, result);
    throw new Error("Invalid data structure from WASM parser.");
  }

  console.log(
    `${LOG_PREFIX} Parsed ${result.rules.length} rules. Stats: ` +
    `totalLines=${result.stats.totalLines}, ` +
    `processed=${result.stats.processedRules}, ` +
    `skipped=${result.stats.skippedLines}`
  );
  return result; // Gibt das ganze Objekt zurück, inkl. Stats
}

// === Kernlogik ===

/**
 * Initialisiert die Erweiterung: Lädt WASM, holt Filterliste, parst sie und wendet Regeln an.
 * Verwendet einen Lock, um parallele Ausführungen zu verhindern.
 */
async function initialize() {
  // Prüfen, ob bereits eine Initialisierung läuft
  if (isInitializing) {
    console.log(`${LOG_PREFIX} Initialization already in progress. Skipping.`);
    return;
  }
  isInitializing = true; // Lock setzen
  console.log(`${LOG_PREFIX} Starting initialization...`);
  await clearBadge(); // Badge zu Beginn löschen

  try {
    // 1. WASM-Modul laden/sicherstellen
    const wasmModule = await ensureWasmModuleLoaded();

    // 2. Filterliste abrufen
    const listText = await fetchFilterList();

    // 3. Liste mit WASM parsen
    const parseResult = parseListWithWasm(wasmModule, listText);
    const rules = parseResult.rules;
    const stats = parseResult.stats; // Statistiken extrahieren

    // 4. Regeln anwenden (via declarativeNetRequest)
    if (rules.length === 0) {
        // Spezieller Fall: Leere Liste oder nur Kommentare/ungültige Regeln
        console.warn(`${LOG_PREFIX} Filter list resulted in 0 rules. Applying empty ruleset.`);
        // Wichtig: updateRules muss mit einem leeren Array umgehen können,
        // um ggf. alte Regeln zu löschen.
        await updateRules([]); // Explizit leeres Array übergeben
        // Optional: Badge setzen, um leere Liste anzuzeigen?
        // await setErrorBadge(BADGE_TEXT_EMPTY_LIST); // Oder nur loggen
        // Speichere 0 als Regelanzahl
        await chrome.storage.local.set({ ruleCount: 0, ruleStats: stats });
    } else {
        console.log(`${LOG_PREFIX} Applying ${rules.length} rules...`);
        // updateRules sollte die Anzahl der erfolgreich angewendeten Regeln zurückgeben oder speichern
        // Wir nehmen an, dass updateRules bei Erfolg die 'ruleCount' im Storage setzt.
        await updateRules(rules); // Fehler hier werden vom äußeren catch gefangen
        // Speichere Statistiken (Anzahl wird von updateRules gesetzt)
        await chrome.storage.local.set({ ruleStats: stats });
    }

    // 5. Erfolg signalisieren (Badge löschen)
    await clearBadge();
    console.log(`${LOG_PREFIX} Initialization complete.`);

  } catch (error) {
    console.error(`${LOG_PREFIX} Initialization failed:`, error);
    // Spezifischeren Fehler-Badge setzen
    let badgeText = BADGE_TEXT_INIT_ERROR;
    if (error.message.includes("WASM")) {
        badgeText = BADGE_TEXT_WASM_ERROR;
    } else if (error.message.includes("Fetch")) {
        badgeText = BADGE_TEXT_FETCH_ERROR;
    } else if (error.message.includes("Parse") || error.message.includes("JSON") || error.message.includes("structure")) {
        badgeText = BADGE_TEXT_PARSE_ERROR;
    } else if (error.message.includes("updateRules") || error.message.includes("Rule application")) { // Annahme: updateRules wirft Fehler mit Kennung
        badgeText = BADGE_TEXT_RULES_ERROR;
    }
    await setErrorBadge(badgeText);
    // Optional: Fehler im Storage speichern für Debugging?
    // await chrome.storage.local.set({ lastError: error.message });

  } finally {
    isInitializing = false; // Lock freigeben, egal ob Erfolg oder Fehler
  }
}

// === Event-Listener ===

// Wird bei der Installation oder einem Update der Erweiterung ausgelöst.
chrome.runtime.onInstalled.addListener((details) => {
  console.log(`${LOG_PREFIX} Extension ${details.reason}.`);
  initialize(); // Starte die Initialisierung
});

// Wird ausgelöst, wenn der Browser startet (und die Erweiterung aktiviert ist).
chrome.runtime.onStartup.addListener(() => {
  console.log(`${LOG_PREFIX} Browser startup detected.`);
  initialize(); // Starte die Initialisierung
});

// Lauscht auf Nachrichten von anderen Teilen der Erweiterung (z.B. Popup).
chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
  console.log(`${LOG_PREFIX} Received message:`, request);

  if (request.action === "getStats") {
    // Asynchrone Antwort erforderlich -> true zurückgeben
    chrome.storage.local.get(['ruleCount', 'ruleStats', 'lastError'])
      .then(data => {
        sendResponse({
            ruleCount: data.ruleCount ?? 'N/A',
            ruleStats: data.ruleStats ?? {},
            lastError: data.lastError // Optional: Letzten Fehler anzeigen
        });
      })
      .catch(err => {
        console.error(`${LOG_PREFIX} Error getting stats from storage:`, err);
        sendResponse({ ruleCount: 'Fehler', ruleStats: {}, error: err.message });
      });
    return true; // Signalisiert asynchrone Antwort
  }

  if (request.action === "reloadRules") {
    // Asynchrone Antwort erforderlich -> true zurückgeben
    console.log(`${LOG_PREFIX} Reloading rules requested via popup...`);
    initialize() // Ruft die (jetzt gesicherte) Initialisierungsfunktion auf
      .then(async () => {
        // Warten, bis initialize() abgeschlossen ist (inkl. Storage-Update)
        // Der setTimeout ist hier wahrscheinlich nicht mehr nötig, da wir auf
        // das Promise von initialize() warten, welches wiederum auf das Promise
        // von updateRules() (und dem darin enthaltenen storage.set) warten sollte.
        // GRÜNDLICH TESTEN, ob der ruleCount sofort korrekt ist!
        // await new Promise(r => setTimeout(r, 200)); // Entfernt - testen!

        // Hole die aktualisierten Daten direkt nach Abschluss von initialize
        const data = await chrome.storage.local.get(['ruleCount', 'ruleStats']);
        sendResponse({
          success: true,
          message: "Rules reloaded successfully.",
          ruleCount: data.ruleCount ?? 'N/A',
          ruleStats: data.ruleStats ?? {}
        });
      })
      .catch(err => {
        // Fehler während des Reloads (wurde schon in initialize geloggt und Badge gesetzt)
        console.error(`${LOG_PREFIX} reloadRules failed:`, err);
        // Sende trotzdem eine Antwort an das Popup
        sendResponse({
            success: false,
            message: `Failed to reload rules: ${err.message}`,
            ruleCount: 'Fehler',
            ruleStats: {}
         });
      });
    return true; // Signalisiert asynchrone Antwort
  }

  // Wenn die Nachricht nicht behandelt wurde
  console.log(`${LOG_PREFIX} Unhandled message action:`, request.action);
  return false; // Keine asynchrone Antwort geplant
});

// === Initialer Start ===
// Starte die Initialisierung direkt beim Laden des Background-Skripts.
// Dies deckt Fälle ab, in denen weder onInstalled noch onStartup feuern
// (z.B. nach einem Absturz des Extension-Prozesses oder beim Debuggen).
console.log(`${LOG_PREFIX} Background script loaded. Triggering initial load.`);
initialize();
