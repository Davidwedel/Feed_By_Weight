#include "auger_control.h"
#include "config.h"

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
    _alarmTriggered = false;
    _chainPreRunTime = 10;
    _maxRuntime = 600;
    _fillSettlingTime = 60;
    _alarmThreshold = 10.0;
    _fillDetectionRate = 20.0;
    _fluctuationThreshold = 2.0;
    _fillRateWeight = 0;
    _fillRateStartTime = 0;
    _weightAtMinuteStart = 0;
    _minuteStartTime = 0;
    _lastValidWeight = 0;
    _weightReadingFailed = false;
    _warningPending = false;
    _warnedWeightFail = false;
    _warnedNoChange = false;
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

void AugerControl::startFeeding(float targetWeight, uint16_t chainPreRunTime, uint16_t maxRuntime, float fillDetectionRate, uint16_t fillSettlingMinutes, float fluctuationThreshold) {
    if (_stage != FeedingStage::STOPPED) {
        Serial.println("Cannot start feeding - already in progress");
        return;
    }

    _targetWeight = targetWeight;
    _chainPreRunTime = chainPreRunTime;
    _maxRuntime = maxRuntime;
    _fillDetectionRate = fillDetectionRate;
    _fluctuationThreshold = fluctuationThreshold;
    _fillSettlingTime = fillSettlingMinutes * 60;  // convert minutes to seconds internally
    _feedStartTime = millis();
    _chainStartTime = millis();
    _lastWeightCheck = millis();
    _minuteStartTime = millis();
    _startWeight = 0;  // Will be set on first update
    _weightDispensed = 0;
    _alarmTriggered = false;
    _warningPending = false;
    _warnedWeightFail = false;
    _warnedNoChange = false;
    _warnedIncrease = false;
    _warnedLowRate = false;
    _lastWeight = 0;
    _fillInProgress = false;
    _fillStabilizedTime = 0;
    _fillRateWeight = 0;
    _fillRateStartTime = 0;
    strcpy(_alarmReason, "");

    // Start with chain only
    _stage = FeedingStage::CHAIN_ONLY;
    Serial.println("About to start chain...");
    controlChain(true);

    Serial.printf("Feeding started: Target=%.2f, ChainPreRun=%ds, MaxTime=%ds\n",
                  targetWeight, chainPreRunTime, maxRuntime);
}

FeedingStage AugerControl::update(float currentTotalWeight) {
    if (_stage == FeedingStage::STOPPED || _stage == FeedingStage::COMPLETED || _stage == FeedingStage::FAILED) {
        return _stage;
    }

    // Check if weight reading failed (0 or negative usually means read error)
    if (currentTotalWeight <= 0) {
        if (!_warnedWeightFail) {
            sendWarning("⚠️ Weight reading failed - continuing until max runtime");
            _warnedWeightFail = true;
        }
        _weightReadingFailed = true;
        // Use last valid weight if available
        if (_lastValidWeight > 0) {
            currentTotalWeight = _lastValidWeight;
        }
    } else {
        // Check if problem cleared
        if (_weightReadingFailed && _warnedWeightFail) {
            sendWarning("✅ Weight reading restored");
            _warnedWeightFail = false;
        }
        _weightReadingFailed = false;
        _lastValidWeight = currentTotalWeight;
    }

    // Initialize start weight on first update
    if (_startWeight == 0 && currentTotalWeight > 0) {
        _startWeight = currentTotalWeight;
        _weightAtMinuteStart = currentTotalWeight;
        Serial.printf("Start weight initialized: %.2f lbs\n", _startWeight);
    }

    // Calculate weight dispensed (weight should decrease as feed goes out)
    _weightDispensed = _startWeight - currentTotalWeight;

    // Rate-based bin fill detection (only if not already paused)
    if (_stage != FeedingStage::PAUSED_FOR_FILL) {
        if (_fillRateStartTime == 0) {
            // Initialize rate tracking window
            _fillRateWeight = currentTotalWeight;
            _fillRateStartTime = millis();
        } else {
            unsigned long rateElapsed = millis() - _fillRateStartTime;
            if (rateElapsed >= 10000) {  // Evaluate rate every 10 seconds
                float weightChange = currentTotalWeight - _fillRateWeight;
                float elapsedMinutes = rateElapsed / 60000.0;
                float ratePerMinute = weightChange / elapsedMinutes;

                if (ratePerMinute > _fillDetectionRate) {
                    // Weight increasing faster than threshold — bin fill detected
                    _stageBeforePause = _stage;
                    controlAuger(false);
                    controlChain(false);
                    _stage = FeedingStage::PAUSED_FOR_FILL;
                    _fillInProgress = true;
                    _weightWhenPaused = currentTotalWeight;
                    _lastWeightDuringPause = currentTotalWeight;
                    _fillStabilizedTime = 0;
                    Serial.printf("Feed PAUSED - bin fill detected (%.1f lb/min > %.1f lb/min threshold)\n",
                                  ratePerMinute, _fillDetectionRate);
                    return _stage;
                }

                // Reset window for next evaluation
                _fillRateWeight = currentTotalWeight;
                _fillRateStartTime = millis();
            }
        }
    }

    unsigned long elapsed = (millis() - _feedStartTime) / 1000;  // seconds

    switch (_stage) {
        case FeedingStage::CHAIN_ONLY:
            // Check if chain pre-run time has elapsed
            if ((millis() - _chainStartTime) / 1000 >= _chainPreRunTime) {
                // Start auger as well
                Serial.printf("Chain pre-run complete (%ds), starting auger...\n", _chainPreRunTime);
                controlAuger(true);
                _stage = FeedingStage::BOTH_RUNNING;

                // Reset timing for safety monitoring to start fresh
                _bothRunningStartTime = millis();
                _minuteStartTime = millis();
                _weightAtMinuteStart = currentTotalWeight;

                Serial.println("Stage: BOTH_RUNNING");
            }
            break;

        case FeedingStage::BOTH_RUNNING:
            // Check safety conditions (sends warnings, doesn't stop)
            checkSafety(currentTotalWeight);
            // Check if target weight reached
            if (_weightDispensed >= _targetWeight) {
                stopAll();
                _stage = FeedingStage::COMPLETED;
                Serial.printf("Feeding completed: Dispensed=%.2f in %lus\n",
                             _weightDispensed, elapsed);
                return _stage;
            }

            // Check for warning condition (insufficient feed rate) - every minute
            if (millis() - _minuteStartTime >= 60000) {
                float weightPerMinute = _weightAtMinuteStart - currentTotalWeight;

                if (weightPerMinute < _alarmThreshold) {
                    if (!_warnedLowRate) {
                        sendWarning("⚠️ Low feed rate - bin may be empty or jammed");
                        _warnedLowRate = true;
                    }
                } else if (_warnedLowRate) {
                    // Feed rate improved
                    sendWarning("✅ Feed rate normal");
                    _warnedLowRate = false;
                }

                _weightAtMinuteStart = currentTotalWeight;
                _minuteStartTime = millis();
            }

            // Check maximum runtime - ONLY failure condition
            if (elapsed >= _maxRuntime) {
                triggerAlarm("Maximum runtime exceeded");
            }
            break;

        case FeedingStage::PAUSED_FOR_FILL:
            // Monitor weight to detect when filling stops
            if (currentTotalWeight > _lastWeightDuringPause + _fluctuationThreshold) {
                // Weight still increasing - reset stabilization timer
                _lastWeightDuringPause = currentTotalWeight;  // Update tracking weight
                _fillStabilizedTime = 0;
            } else {
                // Weight stable or decreasing
                if (_fillStabilizedTime == 0) {
                    // Start settling countdown
                    _fillStabilizedTime = millis();
                }

                // Check if settling time has elapsed
                unsigned long settleElapsed = (millis() - _fillStabilizedTime) / 1000;
                if (settleElapsed >= _fillSettlingTime) {
                    // Resume feeding
                    _fillInProgress = false;

                    // Adjust baseline weight to preserve already-dispensed amount
                    // Calculate gain from when we paused, not from original start
                    float weightGain = currentTotalWeight - _weightWhenPaused;
                    _startWeight += weightGain;  // Add the gained weight to baseline

                    // Reset tracking to prevent immediate re-trigger
                    _lastWeight = currentTotalWeight;
                    _fillRateWeight = currentTotalWeight;
                    _fillRateStartTime = millis();

                    Serial.printf("Feed RESUMED after bin fill (+%.2f lbs, settled for %ds)\n",
                                 weightGain, _fillSettlingTime);

                    // Resume to previous stage
                    _stage = _stageBeforePause;

                    // Restart appropriate motors
                    if (_stage == FeedingStage::CHAIN_ONLY) {
                        controlChain(true);
                    } else if (_stage == FeedingStage::BOTH_RUNNING) {
                        controlChain(true);
                        controlAuger(true);
                        // Reset monitoring timers for resumed feeding
                        _bothRunningStartTime = millis();
                        _minuteStartTime = millis();
                        _weightAtMinuteStart = currentTotalWeight;
                    }

                    // Return immediately to prevent re-executing resume logic
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

void AugerControl::checkSafety(float currentWeight) {
    // Calculate elapsed time from when BOTH_RUNNING started (not from chain pre-run)
    unsigned long elapsed = (millis() - _bothRunningStartTime) / 1000;

    // Check: no weight change after 30 seconds
    if (elapsed > 30 && _weightDispensed < _fluctuationThreshold) {
        if (!_warnedNoChange) {
            sendWarning("⚠️ No weight change detected - bin may be empty or jammed");
            _warnedNoChange = true;
        }
    } else if (_warnedNoChange && _weightDispensed >= _fluctuationThreshold) {
        // Weight started changing
        sendWarning("✅ Weight dispensing resumed");
        _warnedNoChange = false;
    }
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
    _stage = FeedingStage::FAILED;
}

void AugerControl::sendWarning(const char* warning) {
    strncpy(_warningMessage, warning, sizeof(_warningMessage) - 1);
    _warningMessage[sizeof(_warningMessage) - 1] = '\0';
    _warningPending = true;
    Serial.printf("WARNING: %s\n", warning);
}

float AugerControl::getFlowRate() const {
    unsigned long elapsed = getDuration();
    if (elapsed == 0) return 0;

    float elapsedMinutes = elapsed / 60.0;
    return _weightDispensed / elapsedMinutes;
}

unsigned long AugerControl::getDuration() const {
    if (_feedStartTime == 0) return 0;

    if (_stage == FeedingStage::STOPPED || _stage == FeedingStage::COMPLETED || _stage == FeedingStage::FAILED) {
        return _lastWeightCheck / 1000;  // Return final duration
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
