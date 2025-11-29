#ifndef AUGER_CONTROL_H
#define AUGER_CONTROL_H

#include <Arduino.h>
#include "types.h"

class AugerControl {
public:
    AugerControl();

    // Initialize relay pins
    void begin();

    // Start feeding cycle
    void startFeeding(float targetWeight, uint16_t auger2PreRunTime, uint16_t maxRuntime);

    // Stop all augers immediately
    void stopAll();

    // Update - call frequently in main loop
    // Returns current feeding stage
    FeedingStage update(float currentTotalWeight);

    // Get status
    bool isAuger1Running() const { return _auger1Running; }
    bool isAuger2Running() const { return _auger2Running; }
    FeedingStage getStage() const { return _stage; }
    float getWeightDispensed() const { return _weightDispensed; }
    unsigned long getDuration() const;
    bool isAlarmTriggered() const { return _alarmTriggered; }
    const char* getAlarmReason() const { return _alarmReason; }

    // Manual control
    void setAuger1(bool state);
    void setAuger2(bool state);

    // Check if feeding is active
    bool isFeeding() const { return _stage != FeedingStage::STOPPED; }

private:
    bool _auger1Running;
    bool _auger2Running;
    FeedingStage _stage;

    float _targetWeight;
    float _startWeight;
    float _weightDispensed;
    float _alarmThreshold;

    uint16_t _auger2PreRunTime;  // How long auger 2 runs alone (seconds)
    uint16_t _maxRuntime;

    unsigned long _feedStartTime;
    unsigned long _auger2StartTime;
    unsigned long _lastWeightCheck;

    bool _alarmTriggered;
    char _alarmReason[64];

    // Weight change tracking for alarm
    float _weightAtMinuteStart;
    unsigned long _minuteStartTime;

    // Safety
    void checkSafety(float currentWeight);
    void triggerAlarm(const char* reason);

    // Low-level relay control
    void controlRelay1(bool state);
    void controlRelay2(bool state);
};

#endif // AUGER_CONTROL_H
