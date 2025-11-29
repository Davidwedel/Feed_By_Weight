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
    _lastWeightCheck = 0;
    _alarmTriggered = false;
    _chainPreRunTime = 10;
    _maxRuntime = 600;
    _alarmThreshold = 10.0;
    _weightAtMinuteStart = 0;
    _minuteStartTime = 0;
    strcpy(_alarmReason, "");
}

void AugerControl::begin() {
    // Configure relay pins as outputs
    pinMode(RELAY_1_PIN, OUTPUT);
    pinMode(RELAY_2_PIN, OUTPUT);

    // Ensure all relays are OFF at startup
    stopAll();

    Serial.println("Auger and chain control initialized");
}

void AugerControl::startFeeding(float targetWeight, uint16_t chainPreRunTime, uint16_t maxRuntime) {
    if (_stage != FeedingStage::STOPPED) {
        Serial.println("Cannot start feeding - already in progress");
        return;
    }

    _targetWeight = targetWeight;
    _chainPreRunTime = chainPreRunTime;
    _maxRuntime = maxRuntime;
    _feedStartTime = millis();
    _chainStartTime = millis();
    _lastWeightCheck = millis();
    _minuteStartTime = millis();
    _startWeight = 0;  // Will be set on first update
    _weightDispensed = 0;
    _alarmTriggered = false;
    strcpy(_alarmReason, "");

    // Start with chain only
    _stage = FeedingStage::CHAIN_ONLY;
    controlChain(true);

    Serial.printf("Feeding started: Target=%.2f, ChainPreRun=%ds, MaxTime=%ds\n",
                  targetWeight, chainPreRunTime, maxRuntime);
}

FeedingStage AugerControl::update(float currentTotalWeight) {
    if (_stage == FeedingStage::STOPPED || _stage == FeedingStage::COMPLETED || _stage == FeedingStage::FAILED) {
        return _stage;
    }

    // Initialize start weight on first update
    if (_startWeight == 0 && currentTotalWeight > 0) {
        _startWeight = currentTotalWeight;
        _weightAtMinuteStart = currentTotalWeight;
    }

    // Calculate weight dispensed (weight should decrease as feed goes out)
    _weightDispensed = _startWeight - currentTotalWeight;

    // Check safety conditions
    checkSafety(currentTotalWeight);

    if (_alarmTriggered) {
        stopAll();
        _stage = FeedingStage::FAILED;
        return _stage;
    }

    unsigned long elapsed = (millis() - _feedStartTime) / 1000;  // seconds

    switch (_stage) {
        case FeedingStage::CHAIN_ONLY:
            // Check if chain pre-run time has elapsed
            if ((millis() - _chainStartTime) / 1000 >= _chainPreRunTime) {
                // Start auger as well
                controlAuger(true);
                _stage = FeedingStage::BOTH_RUNNING;
                Serial.println("Stage: BOTH_RUNNING");
            }
            break;

        case FeedingStage::BOTH_RUNNING:
            // Check if target weight reached
            if (_weightDispensed >= _targetWeight) {
                stopAll();
                _stage = FeedingStage::COMPLETED;
                Serial.printf("Feeding completed: Dispensed=%.2f in %lus\n",
                             _weightDispensed, elapsed);
                return _stage;
            }

            // Check for alarm condition (insufficient feed rate)
            if (millis() - _minuteStartTime >= 60000) {  // Every minute
                float weightPerMinute = _weightAtMinuteStart - currentTotalWeight;

                if (weightPerMinute < _alarmThreshold) {
                    triggerAlarm("Insufficient feed rate");
                }

                _weightAtMinuteStart = currentTotalWeight;
                _minuteStartTime = millis();
            }

            // Check maximum runtime
            if (elapsed >= _maxRuntime) {
                triggerAlarm("Maximum runtime exceeded");
            }
            break;

        default:
            break;
    }

    return _stage;
}

void AugerControl::stopAll() {
    controlAuger(false);
    controlChain(false);
    _stage = FeedingStage::STOPPED;
}

void AugerControl::checkSafety(float currentWeight) {
    // Safety check: weight should never increase significantly during feeding
    if (currentWeight > _startWeight + 10.0) {
        triggerAlarm("Weight increased during feeding - possible bin filling error");
        return;
    }

    // Check if weight is changing at all
    unsigned long elapsed = (millis() - _feedStartTime) / 1000;
    if (elapsed > 30 && _weightDispensed < MIN_WEIGHT_CHANGE) {
        triggerAlarm("No weight change detected - possible jam or empty bin");
        return;
    }
}

void AugerControl::triggerAlarm(const char* reason) {
    if (_alarmTriggered) return;  // Already triggered

    _alarmTriggered = true;
    strncpy(_alarmReason, reason, sizeof(_alarmReason) - 1);
    _alarmReason[sizeof(_alarmReason) - 1] = '\0';

    Serial.printf("ALARM: %s\n", reason);
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
    _augerRunning = state;
    Serial.printf("Auger: %s\n", state ? "ON" : "OFF");
}

void AugerControl::controlChain(bool state) {
    digitalWrite(RELAY_2_PIN, state ? HIGH : LOW);
    _chainRunning = state;
    Serial.printf("Chain: %s\n", state ? "ON" : "OFF");
}
