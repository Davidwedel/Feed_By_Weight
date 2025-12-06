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
    void startFeeding(float targetWeight, uint16_t chainPreRunTime, uint16_t maxRuntime);

    // Stop all immediately
    void stopAll();

    // Update - call frequently in main loop
    // Returns current feeding stage
    FeedingStage update(float currentTotalWeight);

    // Get status
    bool isAugerRunning() const { return _augerRunning; }
    bool isChainRunning() const { return _chainRunning; }
    FeedingStage getStage() const { return _stage; }
    float getWeightDispensed() const { return _weightDispensed; }
    unsigned long getDuration() const;
    bool isAlarmTriggered() const { return _alarmTriggered; }
    const char* getAlarmReason() const { return _alarmReason; }

    // Manual control
    void setAuger(bool state);
    void setChain(bool state);

    // Check if feeding is active (only active stages, not terminal states)
    bool isFeeding() const {
        return _stage == FeedingStage::CHAIN_ONLY || _stage == FeedingStage::BOTH_RUNNING;
    }

private:
    bool _augerRunning;
    bool _chainRunning;
    FeedingStage _stage;

    float _targetWeight;
    float _startWeight;
    float _weightDispensed;
    float _alarmThreshold;

    uint16_t _chainPreRunTime;  // How long chain runs alone (seconds)
    uint16_t _maxRuntime;

    unsigned long _feedStartTime;
    unsigned long _chainStartTime;
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
    void controlAuger(bool state);
    void controlChain(bool state);
};

#endif // AUGER_CONTROL_H
