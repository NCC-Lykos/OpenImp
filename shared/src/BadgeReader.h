// Reads badge IDs from an RFIDeas (Imprivata) reader through the pcProx SDK.
// The SDK keeps global state, so use one BadgeReader from one thread only.

#pragma once

#include <string>

class BadgeReader
{
public:
    ~BadgeReader();

    bool Connect();
    void Disconnect();
    bool IsConnected() const { return _connected; }

    // Returns the badge currently on the reader as "<bits>-<hex>", for example
    // "26-02B453FF", or an empty string when no badge is present.
    std::wstring ReadBadge();

private:
    bool _connected = false;
};
