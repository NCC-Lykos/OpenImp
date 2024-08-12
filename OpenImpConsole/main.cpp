#include <iostream>
#include <Windows.h>
#include "..\shared\headers\RFIDeas_pcProxAPI.h"

int main() {
    std::cout << "Initializing the RFIDeas card reader..." << std::endl;

    unsigned short connectionResult = usbConnect();

    if (connectionResult == 1) {
        std::cout << "RFIDeas card reader connected successfully." << std::endl;

        while (true) {
            short bufferSize = 64;
            short cardDataLengthBytes = static_cast<short>(getActiveID(bufferSize) / 8);

            if (cardDataLengthBytes > 1) {
                for (short i = 0; i < cardDataLengthBytes; ++i) {
                    std::cout << std::hex << (int)getActiveID_byte(i) << " ";
                }
                std::cout << "\nCard Data Length: " << cardDataLengthBytes << std::endl;
            }
            Sleep(500);
        }
    }
    else {
        std::cout << "Failed to connect to RFIDeas card reader. Error code: " << connectionResult << std::endl;
    }

    return 0;
}
