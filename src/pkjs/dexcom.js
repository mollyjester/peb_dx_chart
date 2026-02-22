//Credits: https://github.com/gagebenne/pydexcom
// ES5 compatible version

// Constants
var DEXCOM_APPLICATION_ID_US = 'd89443d2-327c-4a6f-89e5-496bbb0317db';
var DEXCOM_APPLICATION_ID_OUS = DEXCOM_APPLICATION_ID_US;
var DEXCOM_APPLICATION_ID_JP = 'd8665ade-9673-4e27-9ff6-92db4ce13d13';

var DEXCOM_BASE_URL = 'https://share2.dexcom.com/ShareWebServices/Services/';
var DEXCOM_BASE_URL_OUS = 'https://shareous1.dexcom.com/ShareWebServices/Services/';
var DEXCOM_BASE_URL_JP = 'https://share.dexcom.jp/ShareWebServices/Services/';

var DEXCOM_AUTHENTICATE_ENDPOINT = "General/AuthenticatePublisherAccount";
var DEXCOM_LOGIN_ID_ENDPOINT = "General/LoginPublisherAccountById";
var DEXCOM_GLUCOSE_READINGS_ENDPOINT = "Publisher/ReadPublisherLatestGlucoseValues";

var Regions = {
    US: 'us',
    OUS: 'ous',
    JP: 'jp'
};

var BaseURLs = {
    us: DEXCOM_BASE_URL,
    ous: DEXCOM_BASE_URL_OUS,
    jp: DEXCOM_BASE_URL_JP
};

var AppIDs = {
    us: DEXCOM_APPLICATION_ID_US,
    ous: DEXCOM_APPLICATION_ID_OUS,
    jp: DEXCOM_APPLICATION_ID_JP
};

/**
 * Dexcom constructor
 * @param {string} username - Dexcom username
 * @param {string} password - Dexcom password
 * @param {Function} onResults - Callback on successful glucose fetch
 * @param {string} region - Region code (us, ous, jp)
 * @param {Function} onError - Callback on fetch error (optional)
 */
function Dexcom(username, password, onResults, region, onError) {
    this.username = username;
    this.password = password;
    this.region = (region || Regions.OUS).toLowerCase();
    this.baseUrl = BaseURLs[this.region] || BaseURLs.ous;
    this.applicationId = AppIDs[this.region] || AppIDs.ous;
    this.sessionId = null;
    this.accountId = null;
    this.onResults = onResults;
    this.onError = onError || null;
}

/**
 * Make XHR request
 * @param {string} method - HTTP method
 * @param {string} url - Request URL
 * @returns {XMLHttpRequest} XHR object
 */
Dexcom.prototype.xhr = function(method, url) {
    var req = new XMLHttpRequest();
    req.open(method, url, true);
    req.setRequestHeader('Content-Type', 'application/json');
    req.setRequestHeader('Accept', 'application/json');
    req.setRequestHeader('User-Agent', 'Dexcom Share/3.0.2.11');
    return req;
};

/**
 * Trim quotes from string
 * @param {string} str - String to trim
 * @returns {string} Trimmed string
 */
Dexcom.prototype.trimQuotes = function(str) {
    return str.replace(/"/g, '');
};

/**
 * Format a glucose reading
 * @param {Object} reading - Raw reading object
 * @returns {Object} Formatted reading
 */
Dexcom.prototype.formatReading = function(reading) {
    var TREND_ARROWS = {
        None: '→',
        DoubleUp: '↑↑',
        SingleUp: '↑',
        FortyFiveUp: '↗',
        Flat: '→',
        FortyFiveDown: '↘',
        SingleDown: '↓',
        DoubleDown: '↓↓',
        NotComputable: '?',
        RateOutOfRange: '⚠️'
    };

    return {
        _json: {
            WT: reading.WT,
            ST: reading.ST,
            DT: reading.DT,
            Value: reading.Value,
            Trend: reading.Trend
        },
        _value: reading.Value,
        _trend_direction: reading.Trend,
        _trend_arrow: TREND_ARROWS[reading.Trend] || '?',
        _datetime: new Date(parseInt(reading.WT.match(/\d+/)[0])),
        _status: this.getGlucoseStatus(reading.Value)
    };
};

/**
 * Get glucose status
 * @param {number} value - BG value
 * @returns {string} Status (LOW, HIGH, IN RANGE)
 */
Dexcom.prototype.getGlucoseStatus = function(value) {
    if (value < 70) return 'LOW';
    if (value > 180) return 'HIGH';
    return 'IN RANGE';
};

/**
 * Get trend description from delta
 * @param {number} delta - BG delta value
 * @returns {string} Trend description
 */
Dexcom.prototype.getTrendDescription = function(delta) {
    if (delta === null) return 'Unknown';
    if (delta > 15) return 'Rising quickly';
    if (delta > 7) return 'Rising';
    if (delta > 3) return 'Rising slowly';
    if (delta >= -3) return 'Stable';
    if (delta >= -7) return 'Dropping slowly';
    if (delta >= -15) return 'Dropping';
    return 'Dropping quickly';
};

/**
 * Handle XHR response
 * @param {number} status - HTTP status
 * @param {string} responseText - Response body
 * @returns {Object} Parsed response
 */
Dexcom.prototype.parseResponse = function(status, responseText) {
    if (status === 200) {
        return {
            ok: true,
            data: this.trimQuotes(responseText)
        };
    } else if (status === 500) {
        return {
            ok: false,
            isServerError: true,
            data: JSON.parse(responseText)
        };
    }
    return {
        ok: false,
        isServerError: false,
        status: status
    };
};

/**
 * Authenticate with Dexcom API
 * @param {Function} callback - Callback when done
 */
Dexcom.prototype.authenticate = function(callback) {
    var self = this;
    
    try {
        // Step 1: Get account ID if not already set
        if (!this.accountId) {
            console.log('Getting account ID...');
            this._getAccountId(function() {
                // Step 2: Get session ID
                self._getSessionId(callback);
            });
        } else if (!this.sessionId) {
            // Step 2: Get session ID only
            this._getSessionId(callback);
        } else {
            // Already authenticated
            callback.call(self);
        }
    } catch (error) {
        console.error('Authentication error: ' + error.message);
        if (this.onError) this.onError('Authentication error: ' + error.message);
    }
};

/**
 * Get account ID from Dexcom API
 * @param {Function} callback - Callback when complete
 */
Dexcom.prototype._getAccountId = function(callback) {
    var self = this;
    var authUrl = this.baseUrl + DEXCOM_AUTHENTICATE_ENDPOINT;
    var req = this.xhr('POST', authUrl);

    req.onload = function() {
        if (req.readyState !== 4) return;

        if (req.status === 200) {
            self.accountId = self.trimQuotes(req.responseText);
            console.log('Account ID: ' + self.accountId);

            if (self.accountId === '00000000-0000-0000-0000-000000000000') {
                console.error('Invalid credentials');
                if (self.onError) self.onError('Invalid credentials');
                return;
            }

            callback.call(self);
        } else {
            console.error('Error fetching account ID: ' + req.status);
            if (self.onError) self.onError('Error fetching account ID: ' + req.status);
        }
    };

    req.onerror = function() {
        console.error('Network error fetching account ID');
        if (self.onError) self.onError('Network error fetching account ID');
    };

    req.ontimeout = function() {
        console.error('Timeout fetching account ID');
        if (self.onError) self.onError('Timeout fetching account ID');
    };

    req.send(JSON.stringify({
        accountName: this.username,
        password: this.password,
        applicationId: this.applicationId
    }));
};

/**
 * Get session ID from Dexcom API
 * @param {Function} callback - Callback when complete
 */
Dexcom.prototype._getSessionId = function(callback) {
    var self = this;
    var loginUrl = this.baseUrl + DEXCOM_LOGIN_ID_ENDPOINT;
    var loginReq = this.xhr('POST', loginUrl);
    var timeoutHandle = null;

    // Set 15 second timeout for session ID fetch
    loginReq.timeout = 15000;

    loginReq.onload = function() {
        if (timeoutHandle) clearTimeout(timeoutHandle);
        if (loginReq.readyState !== 4) return;

        if (loginReq.status === 200) {
            self.sessionId = self.trimQuotes(loginReq.responseText);
            console.log('Session ID: ' + self.sessionId);

            if (self.sessionId === '00000000-0000-0000-0000-000000000000') {
                console.error('Login failed');
                if (self.onError) self.onError('Login failed');
                return;
            }

            callback.call(self);
        } else {
            console.error('Error fetching session ID: ' + loginReq.status);
            if (self.onError) self.onError('Error fetching session ID: ' + loginReq.status);
        }
    };

    loginReq.onerror = function() {
        if (timeoutHandle) clearTimeout(timeoutHandle);
        console.error('Network error fetching session ID');
        if (self.onError) self.onError('Network error fetching session ID');
    };

    loginReq.ontimeout = function() {
        if (timeoutHandle) clearTimeout(timeoutHandle);
        console.error('Timeout fetching session ID (15s)');
        if (self.onError) self.onError('Timeout fetching session ID');
    };

    // Fallback timeout using setTimeout for better compatibility
    timeoutHandle = setTimeout(function() {
        if (loginReq.readyState !== 4) {
            console.error('Request timeout: session ID fetch took too long');
            loginReq.abort();
        }
    }, 15000);

    loginReq.send(JSON.stringify({
        accountId: this.accountId,
        password: this.password,
        applicationId: this.applicationId
    }));
};

/**
 * Get glucose readings for the last N minutes (for chart)
 * @param {number} minutes - Minutes to fetch (default 180 for 3 hours)
 * @param {number} maxCount - Max number of readings (default 36 for 3 hours at 5-min intervals)
 */
Dexcom.prototype.getGlucoseReadings = function(minutes, maxCount) {
    var self = this;
    
    minutes = minutes || 180; // Default 3 hours
    maxCount = maxCount || 36; // Default 36 readings (3 hours at 5-min intervals)
    
    if (!this.sessionId) {
        this.authenticate(function() {
            self._fetchGlucoseReadings(minutes, maxCount);
        });
        return;
    }

    this._fetchGlucoseReadings(minutes, maxCount);
};

/**
 * Fetch glucose readings from Dexcom API
 * @param {number} minutes - Minutes to fetch
 * @param {number} maxCount - Max number of readings
 */
Dexcom.prototype._fetchGlucoseReadings = function(minutes, maxCount) {
    var self = this;
    
    try {
        console.log('Fetching glucose readings for ' + minutes + ' minutes...');

        var url = this.baseUrl + DEXCOM_GLUCOSE_READINGS_ENDPOINT;
        var req = this.xhr('POST', url);
        var timeoutHandle = null;

        // Set 15 second timeout for glucose readings
        req.timeout = 15000;

        req.onload = function() {
            if (timeoutHandle) clearTimeout(timeoutHandle);
            if (req.readyState !== 4) return;

            try {
                if (req.status === 200) {
                    self._handleGlucoseResponse(JSON.parse(req.responseText));
                } else if (req.status === 500) {
                    self._handleServerError(JSON.parse(req.responseText), minutes, maxCount);
                } else {
                    console.error('Failed to get readings: ' + req.status);
                }
            } catch (error) {
                console.error('Error processing response: ' + error.message);
            }
        };

        req.onerror = function() {
            if (timeoutHandle) clearTimeout(timeoutHandle);
            console.error('Network error fetching glucose readings');
            if (self.onError) self.onError('Network error fetching glucose readings');
        };

        req.ontimeout = function() {
            if (timeoutHandle) clearTimeout(timeoutHandle);
            console.error('Timeout fetching glucose readings (15s)');
            if (self.onError) self.onError('Timeout fetching glucose readings');
        };

        // Fallback timeout using setTimeout for better compatibility
        timeoutHandle = setTimeout(function() {
            if (req.readyState !== 4) {
                console.error('Request timeout: glucose readings fetch took too long');
                req.abort();
            }
        }, 15000);

        req.send(JSON.stringify({
            sessionId: this.sessionId,
            minutes: minutes,
            maxCount: maxCount
        }));
    } catch (error) {
        console.error('Error fetching glucose: ' + error.message);
        if (self.onError) self.onError('Error fetching glucose: ' + error.message);
    }
};

/**
 * Handle successful glucose response
 * @param {Array} readings - Array of glucose readings
 */
Dexcom.prototype._handleGlucoseResponse = function(readings) {
    if (!Array.isArray(readings) || readings.length === 0) {
        console.error('No readings available');
        return;
    }

    var formattedReadings = [];
    for (var i = 0; i < readings.length; i++) {
        formattedReadings.push(this.formatReading(readings[i]));
    }

    this.onResults(formattedReadings);
};

/**
 * Handle server error response
 * @param {Object} error - Error object from server
 * @param {number} minutes - Minutes parameter to retry with
 * @param {number} maxCount - Max count parameter to retry with
 */
Dexcom.prototype._handleServerError = function(error, minutes, maxCount) {
    var self = this;
    
    // Track retry attempts to prevent infinite loops
    if (!this.retryCount) {
        this.retryCount = 0;
    }
    
    if (error.Code === 'SessionIdNotFound' || error.Code === 'SessionNotValid') {
        // Limit retries to prevent infinite loop
        if (this.retryCount < 2) {
            this.retryCount++;
            console.log('Session error: ' + error.Code + ', re-authenticating (attempt ' + this.retryCount + ')...');
            this.sessionId = null;
            this.authenticate(function() {
                self.getGlucoseReadings(minutes, maxCount);
            });
        } else {
            console.error('Session error: ' + error.Code + ', max retries reached (' + this.retryCount + ')');
            this.retryCount = 0;
            if (this.onError) this.onError('Session error: ' + error.Code + ', max retries reached');
        }
    } else {
        this.retryCount = 0;
        console.error('Server error: ' + error.Message);
        if (this.onError) this.onError('Server error: ' + error.Message);
    }
};

module.exports = Dexcom;
