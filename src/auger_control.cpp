#include "auger_control.h"
#include "config.h"
#include "types.h"

extern SystemStatus systemStatus;

AugerControl::AugerControl() {
    _augerRunning = false;
    _chainRunning = false;
    _stage = FeedingStage::STOPPED;
    _targetWeight = 0;
    _startWeight = 0;
    _weightDispensed = 0;
    _feedStartTime = 0;
    _chainStartTime = 0;
    _bothRunningStartTime = 0;
    _lastWeightCheck = 0;
    _postAveragingStartTime = 0;
    _alarmTriggered = false;
    _chainPreRunTime = 10;
    _maxRuntime = 600;
    _fillSettlingTime = 60;
    _alarmThreshold = 10.0;
    _fillRateWeight = 0;
    _fillRateStartTime = 0;
    _lastValidWeight = 0;
    _weightReadingFailed = false;
    _warningPending = false;
    _warnedWeightFail = false;
    _warnedIncrease = false;
    _warnedLowRate = false;
    _stageBeforePause = FeedingStage::STOPPED;
    _lastWeight = 0;
    _weightWhenPaused = 0;
    _lastWeightDuringPause = 0;
    _fillStabilizedTime = 0;
    _fillInProgress = false;
    strcpy(_alarmReason, "");
    strcpy(_warningMessage, "");
}

void AugerControl::begin() {
    // Configure relay pins as outputs
    pinMode(RELAY_1_PIN, OUTPUT);
    pinMode(RELAY_2_PIN, OUTPUT);
    pinMode(RELAY_3_PIN, OUTPUT);
    pinMode(RELAY_4_PIN, OUTPUT);
    pinMode(RELAY_5_PIN, OUTPUT);
    pinMode(RELAY_6_PIN, OUTPUT);

    // Ensure all relays are OFF at startup
    stopAll();

    Serial.println("Auger and chain control initialized");
}

/**
 * START FEEDING SEQUENCE
 *
 * Initiates a 2-stage feeding sequence:
 * 1. Chain runs alone for chainPreRunTime seconds (moves feed toward auger)
 * 2. Both auger + chain run together until targetWeight is dispensed
 *
 * Also configures:
 * - Bin fill detection (pauses feeding if weight increases rapidly)
 * - Safety monitoring (low flow rate warnings, max runtime alarm)
 * - Warning flags (reset for new feeding cycle)
 */
void AugerControl::startFeeding(float targetWeight, uint16_t chainPreRunTime, uint16_t maxRuntime, uint16_t fillSettlingMinutes) {
    if (_stage != FeedingStage::STOPPED) {
        Serial.println("Cannot start feeding - already in progress");
        return;
    }

    // Store feeding parameters
    _targetWeight = targetWeight;
    _chainPreRunTime = chainPreRunTime;
    _maxRuntime = maxRuntime;
    _fillSettlingTime = fillSettlingMinutes * 60;      // convert minutes to seconds internally

    // Initialize timing
    _feedStartTime = millis();
    _chainStartTime = millis();
    _lastWeightCheck = millis();

    // Calculate start weight as average of last N readings from history
    _startWeight = 0;

    if (systemStatus.historyCount > 0) {
        for (int i = 0; i < systemStatus.historyCount; i++) {
            int idx = (systemStatus.historyIndex - 1 - i + SystemStatus::WEIGHT_HISTORY_SIZE) % SystemStatus::WEIGHT_HISTORY_SIZE;
            _startWeight += systemStatus.weightHistory[idx];
        }
        _startWeight /= systemStatus.historyCount;
        Serial.printf("Start weight: %.2f lbs (average of %d samples)\n", _startWeight, systemStatus.historyCount);
    } else {
        // No history available - use current total weight
        _startWeight = systemStatus.totalCurrentWeight;
        Serial.printf("Start weight: %.2f lbs (no history available)\n", _startWeight);
    }

    _weightDispensed = 0;
    _lastWeight = _startWeight;

    // Reset all warning/alarm flags for new feeding cycle
    _alarmTriggered = false;
    _warningPending = false;
    _warnedWeightFail = false;
    _warnedIncrease = false;
    _warnedLowRate = false;
    strcpy(_alarmReason, "");

    // Reset bin fill detection tracking
    _fillInProgress = false;
    _fillStabilizedTime = 0;
    _fillRateWeight = 0;
    _fillRateStartTime = 0;

    // ========================================
    // Stage 1: Start chain only (auger stays off)
    // ========================================
    _stage = FeedingStage::CHAIN_ONLY;
    Serial.println("About to start chain...");
    controlChain(true);

    Serial.printf("Feeding started: Target=%.2f, ChainPreRun=%ds, MaxTime=%ds\n",
                  targetWeight, chainPreRunTime, maxRuntime);
}

/**
 * UPDATE FEEDING STATE MACHINE
 *
 * Called every loop (~100Hz) from main.cpp while in FEEDING state.
 * Manages the feeding sequence, monitors safety conditions, detects bin fills.
 *
 * State flow:
 * CHAIN_ONLY → (timer) → BOTH_RUNNING → (target reached) → POST_AVERAGING → (60s) → COMPLETED
 *                                     → (max runtime) → FAILED
 *                                     → (bin fill detected) → PAUSED_FOR_FILL → resume
 *
 * Returns current stage so main.cpp can handle completion/failure.
 */
FeedingStage AugerControl::update(float currentTotalWeight) {
    // Don't update if not in an active feeding stage
    if (_stage == FeedingStage::STOPPED || _stage == FeedingStage::COMPLETED
        || _stage == FeedingStage::FAILED || _stage == FeedingStage::PAUSED_MANUAL
        || _stage == FeedingStage::TERMINATED) {
        return _stage;
    }

    // ========================================
    // Weight Reading Validation
    // ========================================
    // BinTrac read failures show as 0 or negative. Use last valid weight to continue.
    if (currentTotalWeight <= 0) {
        if (!_warnedWeightFail) {
            sendWarning("Weight reading failed - continuing until max runtime");
            _warnedWeightFail = true;
        }
        _weightReadingFailed = true;
        // Fall back to last valid weight if available
        if (_lastValidWeight > 0) {
            currentTotalWeight = _lastValidWeight;
        }
    } else {
        // Weight reading restored
        if (_weightReadingFailed && _warnedWeightFail) {
            sendWarning("Weight reading restored");
            _warnedWeightFail = false;
        }
        _weightReadingFailed = false;
        _lastValidWeight = currentTotalWeight;
    }

    // Calculate weight dispensed (bins get lighter as feed goes out)
    _weightDispensed = _startWeight - currentTotalWeight;

    // ========================================
    // Bin Fill Detection
    // ========================================
    // If weight is increasing rapidly (someone filling the bins manually or via truck),
    // pause feeding to avoid incorrect weight measurements.

    // Check for bin fill during active feeding stages
    // Skip during PAUSED_FOR_FILL and POST_AVERAGING (POST_AVERAGING handles bin fills differently)
    if (_stage != FeedingStage::PAUSED_FOR_FILL && _stage != FeedingStage::POST_AVERAGING) {
        if (detectBinFill()) {
            // Bin fill detected - pause feeding until weight stabilizes
            _stageBeforePause = _stage;
            controlAuger(false);
            controlChain(false);
            _stage = FeedingStage::PAUSED_FOR_FILL;
            _fillInProgress = true;
            _weightWhenPaused = currentTotalWeight;
            _lastWeightDuringPause = currentTotalWeight;
            _fillStabilizedTime = 0;
            Serial.printf("Feed PAUSED - bin fill detected (5 consecutive weight increases)\n");
            return _stage;
        }
    }

    unsigned long elapsed = (millis() - _feedStartTime) / 1000;  // Total elapsed time in seconds

    // ========================================
    // Feeding Stage State Machine
    // ========================================
    switch (_stage) {
        case FeedingStage::CHAIN_ONLY:
            // ========================================
            // Stage 1: Chain running alone (auger still off)
            // ========================================
            // Purpose: Move feed along the chain toward the auger inlet before
            // starting the auger. Prevents auger from running empty.
            if ((millis() - _chainStartTime) / 1000 >= _chainPreRunTime) {
                // Pre-run time elapsed - start auger too
                Serial.printf("Chain pre-run complete (%ds), starting auger...\n", _chainPreRunTime);
                controlAuger(true);
                _stage = FeedingStage::BOTH_RUNNING;

                // Reset timing for safety monitoring (start fresh from when both running)
                _bothRunningStartTime = millis();

                Serial.println("Stage: BOTH_RUNNING");
            }
            break;

        case FeedingStage::BOTH_RUNNING:
            {
            // ========================================
            // Stage 2: Both auger + chain running
            // ========================================
            // Main feeding stage. Monitor weight dispensed and check for completion.

            // Check if target weight reached (SUCCESS CONDITION)
            if (_weightDispensed >= _targetWeight) {
                stopAll();
                _lastWeightCheck = millis();  // Record when motors stopped
                _postAveragingStartTime = millis();
                _stage = FeedingStage::POST_AVERAGING;
                Serial.printf("Target reached: Dispensed=%.2f in %lus. Starting post-averaging (60s)...\n",
                             _weightDispensed, elapsed);
                return _stage;
            }

            // ========================================
            // Low Feed Rate Warning
            // ========================================
            // Uses flow rate calculated from weight history (oldest to newest).
            // If dispensing less than threshold lb/min, warn but don't stop.
            // Could indicate bin getting empty or mechanical jam.
            // Wait at least 60 seconds after auger starts before checking.
            unsigned long augerElapsed = (millis() - _bothRunningStartTime) / 1000;
            if (augerElapsed >= 60 && systemStatus.historyCount >= SystemStatus::WEIGHT_HISTORY_SIZE) {
                if (systemStatus.flowRate < _alarmThreshold) {
                    if (!_warnedLowRate) {
                        sendWarning("Low feed rate - bin may be empty or jammed");
                        _warnedLowRate = true;
                    }
                } else if (_warnedLowRate) {
                    // Feed rate improved - clear warning
                    sendWarning("Feed rate normal");
                    _warnedLowRate = false;
                }
            }

            // ========================================
            // Maximum Runtime Check (FAILURE CONDITION)
            // ========================================
            // Only hard failure condition. Prevents infinite feeding if something
            // goes wrong (broken scale, jammed auger, etc.)
            if (elapsed >= _maxRuntime) {
                triggerAlarm("Maximum runtime exceeded");
            }
            }
            break;

        case FeedingStage::PAUSED_FOR_FILL:
            // ========================================
            // Paused due to bin fill in progress
            // ========================================
            // Motors are stopped. Monitor weight to detect when filling finishes,
            // then wait for settling time before resuming.

            if (detectBinFill()) {
                // Bin fill still in progress (5 consecutive weight increases detected)
                _fillStabilizedTime = 0;  // Reset settling timer
            } else {
                // Bin fill stopped (no longer seeing progressive increases)
                if (_fillStabilizedTime == 0) {
                    // Start settling countdown (wait for weight to fully settle)
                    _fillStabilizedTime = millis();
                }

                // Check if weight has been stable for the full settling time
                unsigned long settleElapsed = (millis() - _fillStabilizedTime) / 1000;
                if (settleElapsed >= _fillSettlingTime) {
                    // ========================================
                    // Resume feeding after bin fill
                    // ========================================
                    _fillInProgress = false;

                    // Adjust baseline weight to preserve already-dispensed amount.
                    // If bins gained 100 lbs during pause, add 100 to _startWeight
                    // so _weightDispensed calculation remains correct.
                    float weightGain = currentTotalWeight - _weightWhenPaused;
                    _startWeight += weightGain;

                    // Reset tracking to prevent immediate re-trigger of fill detection
                    _lastWeight = currentTotalWeight;
                    _fillRateWeight = currentTotalWeight;
                    _fillRateStartTime = millis();

                    Serial.printf("Feed RESUMED after bin fill (+%.2f lbs, settled for %ds)\n",
                                 weightGain, _fillSettlingTime);

                    // Resume to whatever stage we were in before pause
                    _stage = _stageBeforePause;

                    // Restart appropriate motors based on which stage we're resuming to
                    if (_stage == FeedingStage::CHAIN_ONLY) {
                        controlChain(true);
                    } else if (_stage == FeedingStage::BOTH_RUNNING) {
                        controlChain(true);
                        controlAuger(true);
                        // Reset monitoring timers for resumed feeding
                        _bothRunningStartTime = millis();
                    }

                    // Return immediately to prevent re-executing resume logic
                    return _stage;
                }
            }
            break;

        case FeedingStage::POST_AVERAGING:
            // ========================================
            // Post-Averaging: Motors stopped, waiting for weight to settle
            // ========================================
            // After target weight reached, wait 60 seconds for bins to settle,
            // then use averaged weight for final dispensed calculation.
            // Continue monitoring for bin fills - if detected, use oldest reading.

            {
                unsigned long postElapsed = (millis() - _postAveragingStartTime) / 1000;

                // Check if bin fill started during settling period
                if (detectBinFill()) {
                    // Bin fill detected during settling - use oldest available reading
                    int oldestIdx = (systemStatus.historyIndex - systemStatus.historyCount + SystemStatus::WEIGHT_HISTORY_SIZE) % SystemStatus::WEIGHT_HISTORY_SIZE;
                    float endWeight = systemStatus.weightHistory[oldestIdx];
                    _weightDispensed = _startWeight - endWeight;
                    _stage = FeedingStage::COMPLETED;
                    Serial.printf("Bin fill detected during post-averaging. Using oldest reading. Final dispensed: %.2f lbs\n",
                                 _weightDispensed);
                    return _stage;
                }

                // Check if 60 seconds elapsed
                if (postElapsed >= 60) {
                    // Average last N readings for final weight
                    float endWeight = 0;
                    int samplesToAverage = (systemStatus.historyCount < SystemStatus::WEIGHT_HISTORY_SIZE) ? systemStatus.historyCount : SystemStatus::WEIGHT_HISTORY_SIZE;

                    for (int i = 0; i < samplesToAverage; i++) {
                        int idx = (systemStatus.historyIndex - 1 - i + SystemStatus::WEIGHT_HISTORY_SIZE) % SystemStatus::WEIGHT_HISTORY_SIZE;
                        endWeight += systemStatus.weightHistory[idx];
                    }
                    endWeight /= samplesToAverage;

                    _weightDispensed = _startWeight - endWeight;
                    _stage = FeedingStage::COMPLETED;
                    Serial.printf("Post-averaging complete (averaged %d samples). Final dispensed: %.2f lbs\n",
                                 samplesToAverage, _weightDispensed);
                    return _stage;
                }
            }
            break;

        default:
            break;
    }

    // Update previous weight for next comparison
    _lastWeight = currentTotalWeight;

    return _stage;
}

void AugerControl::stopAll() {
    controlAuger(false);
    controlChain(false);
    _stage = FeedingStage::STOPPED;
}

/**
 * MANUAL PAUSE/RESUME
 *
 * Allows user to pause feeding via web UI (different from auto-pause for bin fill).
 * Remembers which stage we were in and resumes there.
 */
void AugerControl::pauseFeeding() {
    if (_stage != FeedingStage::CHAIN_ONLY && _stage != FeedingStage::BOTH_RUNNING) {
        Serial.println("Cannot pause - not actively feeding");
        return;
    }
    _stageBeforePause = _stage;  // Remember where we were
    controlAuger(false);
    controlChain(false);
    _stage = FeedingStage::PAUSED_MANUAL;
    Serial.println("Feeding paused by user");
}

void AugerControl::resumeFeeding() {
    if (_stage != FeedingStage::PAUSED_MANUAL) {
        Serial.println("Cannot resume - not manually paused");
        return;
    }

    // Restore previous stage
    _stage = _stageBeforePause;

    // Restart motors appropriate for that stage
    if (_stage == FeedingStage::CHAIN_ONLY) {
        controlChain(true);
    } else if (_stage == FeedingStage::BOTH_RUNNING) {
        controlChain(true);
        controlAuger(true);
        // Reset monitoring timers
        _bothRunningStartTime = millis();
    }

    // Reset fill rate tracking to prevent immediate false fill detection
    _fillRateWeight = _lastValidWeight;
    _fillRateStartTime = millis();

    Serial.printf("Feeding resumed to %s\n",
                  _stage == FeedingStage::CHAIN_ONLY ? "CHAIN_ONLY" : "BOTH_RUNNING");
}

void AugerControl::terminate() {
    controlAuger(false);
    controlChain(false);
    _lastWeightCheck = millis();
    _stage = FeedingStage::TERMINATED;
    Serial.println("Feeding terminated by user");
}

/**
 * DETECT BIN FILL
 *
 * Checks if the last 5 weight readings show a progressive increase,
 * indicating bins are being filled (manually or via truck).
 *
 * Returns: true if bin fill detected, false otherwise
 */
bool AugerControl::detectBinFill() {

	//how many readings back to go
	int howFarBack = 5;

    if (systemStatus.historyCount < howFarBack) {
        return false;  // check if we have enough samples
    }

    // Check if last howFarBack readings are progressively heavier
    for (int i = 0; i < howFarBack - 1; i++) {
        int newerIdx = (systemStatus.historyIndex - 1 - i + SystemStatus::WEIGHT_HISTORY_SIZE) % SystemStatus::WEIGHT_HISTORY_SIZE;
        int olderIdx = (systemStatus.historyIndex - 2 - i + SystemStatus::WEIGHT_HISTORY_SIZE) % SystemStatus::WEIGHT_HISTORY_SIZE;

        if (systemStatus.weightHistory[newerIdx] <= systemStatus.weightHistory[olderIdx]) {
            return false;  // Not progressively increasing
        }
    }

    return true; 
}

void AugerControl::triggerAlarm(const char* reason) {
    if (_alarmTriggered) return;  // Already triggered

    _alarmTriggered = true;
    strncpy(_alarmReason, reason, sizeof(_alarmReason) - 1);
    _alarmReason[sizeof(_alarmReason) - 1] = '\0';

    Serial.printf("ALARM: %s\n", reason);

    // Stop all motors immediately
    controlAuger(false);
    controlChain(false);
    _lastWeightCheck = millis();
    _stage = FeedingStage::FAILED;
}

void AugerControl::sendWarning(const char* warning) {
    strncpy(_warningMessage, warning, sizeof(_warningMessage) - 1);
    _warningMessage[sizeof(_warningMessage) - 1] = '\0';
    _warningPending = true;
    Serial.printf("WARNING: %s\n", warning);
}

unsigned long AugerControl::getDuration() const {
    if (_feedStartTime == 0) return 0;

    if (_stage == FeedingStage::STOPPED || _stage == FeedingStage::COMPLETED
        || _stage == FeedingStage::FAILED || _stage == FeedingStage::TERMINATED
        || _stage == FeedingStage::POST_AVERAGING) {
        return (_lastWeightCheck - _feedStartTime) / 1000;
    }

    return (millis() - _feedStartTime) / 1000;
}

void AugerControl::setAuger(bool state) {
    if (_stage != FeedingStage::STOPPED) {
        Serial.println("Cannot manual control - feeding in progress");
        return;
    }
    controlAuger(state);
}

void AugerControl::setChain(bool state) {
    if (_stage != FeedingStage::STOPPED) {
        Serial.println("Cannot manual control - feeding in progress");
        return;
    }
    controlChain(state);
}

/**
 * LOW-LEVEL RELAY CONTROL
 *
 * These functions actually control the GPIO pins connected to relay modules.
 * Each relay controls a motor contactor.
 *
 * Auger: 2 relays (RELAY_1_PIN, RELAY_6_PIN) - both controlled together
 * Chain: 4 relays (RELAY_2_PIN through RELAY_5_PIN) - one per chain feeder line
 */
void AugerControl::controlAuger(bool state) {
    digitalWrite(RELAY_1_PIN, state ? HIGH : LOW);
    digitalWrite(RELAY_6_PIN, state ? HIGH : LOW);
    _augerRunning = state;
    Serial.printf("GPIOs %d,%d (Augers): %s\n", RELAY_1_PIN, RELAY_6_PIN, state ? "ON (HIGH)" : "OFF (LOW)");
}

void AugerControl::controlChain(bool state) {
    digitalWrite(RELAY_2_PIN, state ? HIGH : LOW);
    digitalWrite(RELAY_3_PIN, state ? HIGH : LOW);
    digitalWrite(RELAY_4_PIN, state ? HIGH : LOW);
    digitalWrite(RELAY_5_PIN, state ? HIGH : LOW);
    _chainRunning = state;
    Serial.printf("GPIOs %d,%d,%d,%d (Chains A-D): %s\n", RELAY_2_PIN, RELAY_3_PIN, RELAY_4_PIN, RELAY_5_PIN, state ? "ON (HIGH)" : "OFF (LOW)");
}
