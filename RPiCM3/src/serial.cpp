#include <serial.h>
#include <fcinterface.h>

#include <pigpio.h>
#include <wiringPi.h>
#include <iostream>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

volatile int serialFd;

// int charCount = 0;
// char serialBuffer[100];
// bool wordEnd = false;
// bool coFlag = false;
volatile bool serialConfigured = false;

volatile int throttleInput = 0;

Serial::Serial() {

}

void Serial::setupSerial(char* port, int baud) {
    if ((serialFd = serOpen(port, baud, 0)) < 0) {
        std::cout << "Unable to open serial interface: " << strerror(errno) << '\n';
        fflush(stdout);
    }
    else {
        std::cout << "Opening Serial. FD: " << serialFd << " ID: " << pthread_self() << '\n';
        serialConfigured = true;
    }
}

char *Serial::readLine() {
    if(!serialConfigured) return -1;
    char buffer[300];
    int i = 0;
    for (char thisChar = (char)serReadByte(serialFd); thisChar != '\n'; 
    thisChar = (char)serReadByte(serialFd)) {
        buffer[i] = thisChar;
        i += 1;
    }
    buffer[i] = '\n';
    buffer[i+1] = '\0';
    return buffer;
}

// void readChar() {
//     // char thisChar = serialGetchar(serialFd);
//     char thisChar = (char)serReadByte(serialFd);
    
//     //Check if this character is the end of message
//     if (thisChar == '\n') {
//         wordEnd = true;
//         serialBuffer[charCount] = '\0';
//         charCount = 0;
//         return;
//     }
    
//     //If we just finished a message, start a new one in the buffer
//     else if (wordEnd == true && (int)thisChar >= 48 && (int)thisChar <= 57) {
//         serialBuffer[charCount] = thisChar;
//         charCount += 1;
//         wordEnd = false;
//         return;
//     }

//     //Assign the next character to the current buffer
//     else if ((int)thisChar >= 48 && (int)thisChar <= 57) {
//         serialBuffer[charCount] = thisChar;
//         charCount += 1;
//         return;
//     }
// }

// void readLine() {
//     //while(!wordEnd) readChar();
//     readChar();
//     if (wordEnd) {                                                  //If we have finished a message
//         int data = (int)strtol(serialBuffer, NULL, 10);                     //Convert hex data to decimal
//         if (coFlag && data > 999 && data <= 2000) {                                 //If we have a coefficient and data for PWM is valid
//             //pthread_mutex_lock(&serial_mutex);
//             throttleInput = data;                                            //Set throttle input
//             //pthread_mutex_unlock(&serial_mutex);
//             coFlag = false;
//             // std::cout << throttleInput << endl;
//             // fflush(stdout);
//         }
//         else if (data == 3) coFlag = true;                                  //If data is 3 (throttle coefficient), flag the value
//         memset(serialBuffer,0,sizeof(serialBuffer));
//     }
//     else return;
// }

void *Serial::serialLoop(void*) {
    if(!serialConfigured) setupSerial();
    while(run) {
        readLine();
        //delayMicroseconds(500);
    }
    return NULL;
}