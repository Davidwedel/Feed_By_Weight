#include "auger_control.h"
#include "config.h"

AugerControl::AugerControl() {
    _auger1Running = false;
    _auger2Running = false;
    _stage = FeedingStage::STOPPED;
    _targetWeight = 0;
    _startWeight = 0;
    _weightDispensed = 0;
    _feedStartTime = 0;
    _auger2StartTime = 0;
    _lastWeightCheck = 0;
    _alarmTriggered = false;
    _auger2PreRunTime = 10;
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

    Serial.println("Auger control initialized");
}

void AugerControl::startFeeding(float targetWeight, uint16_t auger2PreRunTime, uint16_t maxRuntime) {
    if (_stage != FeedingStage::STOPPED) {
        Serial.println("Cannot start feeding - already in progress");
        return;
    }

    _targetWeight = targetWeight;
    _auger2PreRunTime = auger2PreRunTime;
    _maxRuntime = maxRuntime;
    _feedStartTime = millis();
    _auger2StartTime = millis();
    _lastWeightCheck = millis();
    _minuteStartTime = millis();
    _startWeight = 0;  // Will be set on first update
    _weightDispensed = 0;
    _alarmTriggered = false;
    strcpy(_alarmReason, "");

    // Start with Auger 2 only
    _stage = FeedingStage::AUGER2_ONLY;
    controlRelay2(true);

    Serial.printf("Feeding started: Target=%.2f, Auger2PreRun=%ds, MaxTime=%ds\n",
                  targetWeight, auger2PreRunTime, maxRuntime);
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
        case FeedingStage::AUGER2_ONLY:
            // Check if auger 2 pre-run time has elapsed
            if ((millis() - _auger2StartTime) / 1000 >= _auger2PreRunTime) {
                // Start auger 1 as well
                controlRelay1(true);
                _stage = FeedingStage::BOTH_AUGERS;
                Serial.println("Stage: BOTH_AUGERS");
            }
            break;

        case FeedingStage::BOTH_AUGERS:
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
    controlRelay1(false);
    controlRelay2(false);
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

void AugerControl::setAuger1(bool state) {
    if (_stage != FeedingStage::STOPPED) {
        Serial.println("Cannot manual control - feeding in progress");
        return;
    }
    controlRelay1(state);
}

void AugerControl::setAuger2(bool state) {
    if (_stage != FeedingStage::STOPPED) {
        Serial.println("Cannot manual control - feeding in progress");
        return;
    }
    controlRelay2(state);
}

void AugerControl::controlRelay1(bool state) {
    digitalWrite(RELAY_1_PIN, state ? HIGH : LOW);
    _auger1Running = state;
    Serial.printf("Auger 1: %s\n", state ? "ON" : "OFF");
}

void AugerControl::controlRelay2(bool state) {
    digitalWrite(RELAY_2_PIN, state ? HIGH : LOW);
    _auger2Running = state;
    Serial.printf("Auger 2: %s\n", state ? "ON" : "OFF");
}
