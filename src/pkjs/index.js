var Dexcom = require('./dexcom');
var Clay = require('pebble-clay');
var clayConfig = require('./config.json');
var clay = new Clay(clayConfig);

var appSettings = {};
var MMOL_CONVERSION_FACTOR = 18.0182;

/**
 * Load settings from local storage
 */
function getSettings() {
    try {
        return JSON.parse(window.localStorage.getItem('clay-settings')) || {};
    } catch (e) {
        console.error('Error parsing settings: ' + e.message);
        return {};
    }
}

/**
 * Convert mg/dL to mmol/L if needed
 */
function convertBGValue(value, units) {
    if (units === 'mmol/L') {
        return Math.round((value / MMOL_CONVERSION_FACTOR) * 10);
    }
    return Math.round(value * 10);
}

/**
 * Send glucose data array to watch
 */
function sendGlucoseData(readings) {
    if (!readings || readings.length === 0) {
        console.log('No readings to send');
        return;
    }

    var bgUnits = appSettings.BG_UNITS || 'mg/dL';
    
    // Prepare arrays for values and timestamps
    var values = [];
    var timestamps = [];
    
    // Process readings (most recent first)
    for (var i = 0; i < Math.min(readings.length, 36); i++) {
        var reading = readings[i];
        values.push(convertBGValue(reading._value, bgUnits));
        timestamps.push(Math.floor(reading._datetime.getTime() / 1000));
    }
    
    console.log('Sending ' + values.length + ' readings to watch');
    console.log('First reading: ' + values[0] / 10 + ' ' + bgUnits + ' at ' + new Date(timestamps[0] * 1000));
    console.log('Last reading: ' + values[values.length - 1] / 10 + ' ' + bgUnits + ' at ' + new Date(timestamps[timestamps.length - 1] * 1000));
    
    // Send count first
    Pebble.sendAppMessage({
        'BG_COUNT': values.length,
        'BG_UNITS': bgUnits
    }, function() {
        console.log('Sent BG count: ' + values.length);
        
        // Send values one at a time
        sendNextReading(values, timestamps, 0);
    }, function(e) {
        console.error('Failed to send BG count: ' + e.error.message);
    });
}

/**
 * Send glucose readings one at a time
 */
function sendNextReading(values, timestamps, index) {
    if (index >= values.length) {
        console.log('All data sent successfully');
        return;
    }
    
    var dictionary = {
        'BG_INDEX': index,
        'BG_VALUE': values[index],
        'BG_TIMESTAMP': timestamps[index]
    };
    
    Pebble.sendAppMessage(dictionary, function() {
        console.log('Sent reading ' + index + ': ' + values[index] / 10);
        // Send next reading after a small delay
        setTimeout(function() {
            sendNextReading(values, timestamps, index + 1);
        }, 100);
    }, function(e) {
        console.error('Failed to send reading ' + index + ': ' + e.error.message);
        // Retry this reading
        setTimeout(function() {
            sendNextReading(values, timestamps, index);
        }, 500);
    });
}

/**
 * Fetch glucose readings from Dexcom
 */
function fetchGlucoseData() {
    console.log('Fetching glucose data...');
    
    if (!appSettings.DEX_LOGIN || !appSettings.DEX_PASSWORD) {
        console.error('No Dexcom credentials configured');
        return;
    }
    
    var accountId = window.localStorage.getItem('dexcom_account_id');
    var sessionId = window.localStorage.getItem('dexcom_session_id');
    
    var dex = new Dexcom(
        appSettings.DEX_LOGIN,
        appSettings.DEX_PASSWORD,
        function(readings) {
            console.log('Received ' + readings.length + ' readings');
            
            // Cache session IDs
            window.localStorage.setItem('dexcom_account_id', dex.accountId);
            window.localStorage.setItem('dexcom_session_id', dex.sessionId);
            
            // Send to watch
            sendGlucoseData(readings);
        },
        appSettings.DEX_REGION || 'ous'
    );
    
    // Restore session if available
    if (accountId && sessionId) {
        dex.accountId = accountId;
        dex.sessionId = sessionId;
    }
    
    try {
        // Fetch last 3 hours of data (180 minutes, max 36 readings)
        dex.getGlucoseReadings(180, 36);
    } catch (error) {
        console.error('Error fetching glucose: ' + error.message);
    }
}

// Listen for when the watchface is opened
Pebble.addEventListener('ready', function() {
    console.log('PebbleKit JS ready!');
    appSettings = getSettings();
    fetchGlucoseData();
});

// Listen for messages from the watch
Pebble.addEventListener('appmessage', function(e) {
    console.log('AppMessage received from watch');
    appSettings = getSettings();
    fetchGlucoseData();
});

// Listen for when settings are closed
Pebble.addEventListener('webviewclosed', function() {
    console.log('Settings closed');
    appSettings = getSettings();
    fetchGlucoseData();
});
