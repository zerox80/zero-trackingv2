// popup/popup.js
const ruleCountEl = document.getElementById('rule-count');
// const blockedCountEl = document.getElementById('blocked-count'); // Auskommentiert wg. Ungenauigkeit
const refreshButton = document.getElementById('refresh-button');

function updateStatsDisplay(stats) {
    ruleCountEl.textContent = stats.ruleCount !== undefined ? stats.ruleCount.toLocaleString() : 'N/A';
    // blockedCountEl.textContent = stats.blockedCount !== undefined ? stats.blockedCount.toLocaleString() : 'N/A';
    console.log("Popup updated with stats:", stats);
}

function fetchStats() {
    ruleCountEl.textContent = 'Lädt...';
    // blockedCountEl.textContent = 'Lädt...';

    // Frage den Background-Script nach aktuellen Statistiken
    chrome.runtime.sendMessage({ action: "getStats" }, (response) => {
        if (chrome.runtime.lastError) {
            console.error("Error fetching stats:", chrome.runtime.lastError.message);
            ruleCountEl.textContent = 'Fehler';
            // blockedCountEl.textContent = 'Fehler';
            return;
        }
        if (response) {
            updateStatsDisplay(response);
        } else {
             console.warn("Did not receive a response from background script.");
             // Hole direkt aus dem Storage als Fallback
             chrome.storage.local.get(['ruleCount', 'blockedCount']).then(stats => {
                 updateStatsDisplay(stats);
             }).catch(err => console.error("Error getting stats from storage:", err));
        }
    });
}

// Event Listener für den Button
refreshButton.addEventListener('click', fetchStats);

// Lade Statistiken, wenn das Popup geöffnet wird
document.addEventListener('DOMContentLoaded', fetchStats);