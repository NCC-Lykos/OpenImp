#include "..\shared\headers\RFIDeas_pcProxAPI.h"
#include <string>

std::string getDeviceName() {
    std::string s;
    for (short i = 0; i < 127; ++i) {
        char c = getDevName_char(i);
        if (c == 0) break;
        s += c;
    }
    return s;
}

std::string getPartNumberString() {
    std::string s;
    for (short i = 0; i < 127; ++i) {
        char c = getPartNumberString_char(i);
        if (c == 0) break;
        s += c;
    }
    return s;
}

unsigned short readDevCfgFmFile(const std::string& fileName) {
    for (short i = 0; i < fileName.length(); i++) {
        readDevCfgFmFile_char(i, fileName[i]);
    }
    return readDevCfgFmFile_char(255, 0);
}

unsigned short writeDevCfgToFile(const std::string& fileName) {
    for (short i = 0; i < fileName.length(); i++) {
        writeDevCfgToFile_char(i, fileName[i]);
    }
    return writeDevCfgToFile_char(255, 0);
}

short chkAddArrival(const std::string& deviceName) {
    for (short i = 0; i < deviceName.length(); i++) {
        chkAddArrival_char(i, deviceName[i]);
    }
    return chkAddArrival_char(255, 0);
}

short chkDelRemoval(const std::string& deviceName) {
    for (short i = 0; i < deviceName.length(); i++) {
        chkDelRemoval_char(i, deviceName[i]);
    }
    return chkDelRemoval_char(255, 0);
}
