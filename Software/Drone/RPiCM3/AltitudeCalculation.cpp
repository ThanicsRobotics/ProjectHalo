//WiringPi Libraries
//#include <wiringPiSPI.h>
#include <wiringPiI2C.h>
#include <wiringSerial.h>
#include <wiringPi.h>

//pigpio Library
#include <pigpio.h>

//Standard Libraries
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include <bits/stdc++.h>
#include <iostream>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <algorithm>
//#include <fcntl.h>
//#include <termios.h>

//POSIX Thread Library
#include <pthread.h>

//I2C address of IO Expander
#define ADDR 0x22
#define INT_PIN 38

//Defining word representations for program readability
#define LOW 0
#define EDGE_FALLING 0
#define HIGH 1
#define EDGE_RISING 1

using namespace std;

//Thread mutex and gyro thread function
//pthread_mutex_t gyro_mutex = PTHREAD_MUTEX_INITIALIZER;
//pthread_mutex_t serial_mutex = PTHREAD_MUTEX_INITIALIZER;
//pthread_mutex_t run_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_t gyroThread, serialThread;
void *gyroLoop(void *void_ptr);
void *serialLoop(void *void_ptr);
bool run = true;

//Terminal signal handler (for ending program via terminal)
void signal_callback_handler(int);

//Serial UART port file descriptor
int serialFd;
int charCount = 0;
char serialBuffer[100];
char gyroBuffer[100];
bool wordEnd = false;
bool coFlag = false;
//int uart0_filestream = -1;
bool serialConfigured = false;
bool spiConfigured = false;
bool authenticated = false;

//CS0 is barometer, CS1 is STM32 flight controller
int SPI_CS = 1;
int i2cFd;
int spiFd;

//Pressure Altitude variables
char baroData[9];
char baroCoefficients[17];

// int pressure;
// int temperature;
int altitude;
int lastAltitude;

//Gyro angle variables
signed int gyroPitch;
signed int gyroRoll;

//Pulse timing variables
int start_time = 0;
int pulse_time = 0;
//struct timespec gettime_now;
bool pulseComplete = false;
unsigned int lastUltrasonicPulse = 0;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//PID gain and limit settings
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int pid_p_gain = 2;                    //Gain setting for the roll P-controller
int pid_i_gain = 0;                     //Gain setting for the roll I-controller
int pid_d_gain = 0;                   //Gain setting for the roll D-controller
int pid_max = 400;                      //Maximum output of the PID-controller (+/-)
int pid_error_temp;
int pid_i_mem, pid_setpoint, pid_output, pid_last_d_error;
int throttleInput = 0;

void shutdown() {
    endl << cout << "Closing Threads and Ports..." << endl;
    run = false;
    delay(1000);
    pthread_join(serialThread, NULL);
    pthread_join(gyroThread, NULL);

    spiClose(spiFd);
    gpioTerminate();
}

//Request gyro angles from STM32F446 flight controller
void getGyroValues() {

    //Gyro pitch and roll are stored in two incoming bytes
    //wiringPiSPIDataRW(SPI_CS, gyroBuffer, 2);
    spiXfer(spiFd, gyroBuffer, gyroBuffer, 2);
    //pthread_mutex_lock(&gyro_mutex);
    gyroPitch = (signed char)gyroBuffer[0];
    gyroRoll = (signed char)gyroBuffer[1];
    //gyroRoll = buffer[0] << 8 | buffer[1];
    //pthread_mutex_unlock(&gyro_mutex);

    //delayMicroseconds(200);
    //cout << gyroRoll << endl;
}

//Handles IO Expander interrupt (measures ultrasonic sensor echo pulse)
void handleEcho() {
    //Get current time
    start_time = micros();

    //Get time when pulse is HIGH
    while(digitalRead(INT_PIN) == HIGH) {
        //Stop if HIGH for 10ms (timeout)
        if ((micros() - start_time) > 100000) {
            pulse_time = 0;
            break;
        }
    }
    //Pulse time is the time difference before and after HIGH pulse
    pulse_time = micros() - start_time;
    pulseComplete = true;
}

void readline() {
    char thisChar = serialGetchar(serialFd);
    
    //Check if this character is the end of message
    if (thisChar == '\n') {
        wordEnd = true;
        serialBuffer[charCount] = '\0';
        charCount = 0;
        return;
    }
    
    //If we just finished a message, start a new one in the buffer
    else if (wordEnd == true) {
        serialBuffer[charCount] = thisChar;
        charCount += 1;
        wordEnd = false;
        return;
    }

    //Assign the next character to the current buffer
    else {
        serialBuffer[charCount] = thisChar;
        charCount += 1;
        return;
    }
}

void handleSerialInterrupt() {
    readline();
    if (wordEnd == true) {                                                  //If we have finished a message
        int data = (int)strtol(serialBuffer, NULL, 10);                     //Convert hex data to decimal
        if (coFlag == true && data > 999 && data <= 2000) {                                 //If we have a coefficient and data for PWM is valid
            //pthread_mutex_lock(&serial_mutex);
            throttleInput = data;                                            //Set throttle input
            //pthread_mutex_unlock(&serial_mutex);
            coFlag = false;
            //cout << throttleInput << endl;
            //fflush(stdout);
        }
        else if (data == 3) coFlag = true;                                  //If data is 3 (throttle coefficient), flag the value
        memset(serialBuffer,0,sizeof(serialBuffer));
    }
    else return;
}

//Utility function for setting individual pin on IO Expander
void digitalIOWrite(int pin, int state) {
    //Figure out port number based on pin number
    int port;
    if (pin < 8) port = 0;
    else if (pin < 18 && pin > 9) port = 1;
    else port = 2;

    //Change output depending on port and pin number
    switch (port) {
        case 0:
            wiringPiI2CWriteReg8(i2cFd, 0x04, state << pin);
            break;
        case 1:
            wiringPiI2CWriteReg8(i2cFd, 0x05, state << (pin - 10));
            break;
        case 2:
            wiringPiI2CWriteReg8(i2cFd, 0x06, state << (pin - 20));
            break;
        default:
            break;
    }
}

void setupSerial() {
    if ((serialFd = serialOpen("/dev/serial0", 9600)) < 0) {
        cout << "Unable to open serial interface: " << strerror(errno) << endl;
        fflush(stdout);
    }
    else serialConfigured = true;

    //wiringPiISR(15, INT_EDGE_FALLING, handleSerialInterrupt);
}

void setupSPI() {
    if (gpioInitialise() < 0) {
        cout << "pigpio Library failed: " << strerror(errno) << endl;
        exit(1);
    }
    signal(SIGINT, signal_callback_handler);
    if ((spiFd = spiOpen(SPI_CS, 1500000, 0)) < 0) {
        cout << "SPI failed: " << strerror(errno) << endl;
        exit(1);
    }
    else spiConfigured = true;
}

//Configures inputs and outputs of IO Expander
void setupIOExpander() {
    i2cFd = wiringPiI2CSetup(ADDR);

    //Configuration bytes (Inputs are 1's, Outputs 0's)
    //Port 0: 01010101
    wiringPiI2CWriteReg8(i2cFd, 0x0C, 0x55);

    //Port 1: 01010101
    wiringPiI2CWriteReg8(i2cFd, 0x0D, 0x55);

    //Port 2: 11000000
    wiringPiI2CWriteReg8(i2cFd, 0x0E, 0xC0);

    //Initialization of IO Expander interrupts
    wiringPiISR(INT_PIN, INT_EDGE_RISING, handleEcho);
}

//Gets distance value (in centimeters) from downward facing sensor
int getUltrasonicData(int sensor, int iterations, unsigned int pulseDelay) {
    int pin;

    //Toggles between downward facing sensor 1 and 2
    switch (sensor) {
        case 1:
            pin = 17;
            break;
        case 2:
            pin = 15;
            break;
        default:
            break;
    }

    //int totalDistance = 0;
    //int invalids = 0;

    int distances[iterations];
    int loops = 0;
    //Takes average of x distance measurements
    while(loops < iterations) {
        while (millis() - lastUltrasonicPulse < pulseDelay) delay(1);

        //Ensuring TRIG pin is LOW
        digitalIOWrite(pin, LOW);

        //Starting TRIG pulse
        digitalIOWrite(pin, HIGH);
        digitalIOWrite(pin, LOW);

        //Wait until pulse is complete (when handleEcho is complete)
        int start = millis();
        while(!pulseComplete) {
            if (millis() - start > 1000) {
                cout << "Pulse Fail" << endl;
                pulse_time = 0;
                break;
            }
        }

        //Calculate distance based on speed of sound and travel time
        int distance = pulse_time * 340 / 10000 / 2;
        pulseComplete = false;
        lastUltrasonicPulse = millis();

        //factor out invalid results
        if (distance >= 0 && distance < 600) {
            distances[loops] = distance;
            loops++;
        }
    }
    sort(distances, distances + iterations);
    return distances[iterations/2];
}

float radian(int degree) {
    return degree * M_PI / 180;
}

//Corrects downward facing distance measurement when vehicle changes attitude
int angleCorrection(int rawDistance) {
    return sqrt(pow(rawDistance, 2) / (1 + pow(tan(radian(gyroPitch)),2) + pow(tan(radian(gyroRoll)),2)));
}

//Making sure the STM32F446 is listening...
void authFlightController() {
    //Reset flight controller using OpenOCD
    system("sudo openocd");

    authenticated = false;
    char buffer[100];
    unsigned int authKey = 0;
    cout << "Authenticating..." << endl;
    int start = millis();
    while(authKey != 0x00F9) {
        //Write to Authentication register
        buffer[0] = 0x00;
        buffer[1] = 0x01;
        //wiringPiSPIDataRW(SPI_CS, buffer, 2);
        spiWrite(spiFd, buffer, 2);
        delay(5);

        //Get Auth Key and send it back
        //wiringPiSPIDataRW(SPI_CS, buffer, 2);
        spiXfer(spiFd, buffer, buffer, 2);
        authKey = buffer[0] << 8 | buffer[1];
        cout << "Key: " << authKey << endl;
        //wiringPiSPIDataRW(SPI_CS, buffer, 2);
        //spiWrite(spiFd, buffer, 2);
        delay(50);
        if (millis() - start > 8000) {
            return;
        }
    }
    cout << "Authenticated" << endl;
    authenticated = true;
}

//Using gyro angles and raw distance, calculate absolute altitude of vehicle
void calculateAbsoluteAltitude() {
    cout << "Gyro Pitch: " << gyroPitch << " | "  << "Gyro Roll: " << gyroRoll;
    int rawDistance = getUltrasonicData(1, 3, 30);
    cout << " | Raw Distance: " << rawDistance;
    altitude = angleCorrection(rawDistance);
    cout << " | Altitude: " << altitude;
}

// void testSensor() {
//     for (int it = 1; it < 12; it += 2) {
//         for (int s = 30; s < 250; s += 30) {
//             int distances[50];
//             for (int i = 0; i < 50; i++) {
//                 int rawDistance = getUltrasonicData(1, it, s);
//                 distances[i] = rawDistance;
//                 cout << ".";
//                 fflush(stdout);
//             }
//             sort(distances, distances + 50);
//             cout << endl << "Average for " << it << " iterations and " << s << " seconds: " 
//             << distances[25] << endl;
//         }
//     }
// }

//Calculate throttle factor for altitude management through PID loop
void calculatePID() {
    if (throttleInput >= 1520 && throttleInput <= 1480) lastAltitude = altitude;

    pid_error_temp = altitude - lastAltitude + (throttleInput - 1500)/10;
    pid_i_mem += pid_i_gain * pid_error_temp;
    if(pid_i_mem > pid_max)pid_i_mem = pid_max;
    else if(pid_i_mem < pid_max * -1)pid_i_mem = pid_max * -1;

    pid_output = pid_p_gain * pid_error_temp + pid_i_mem + pid_d_gain * (pid_error_temp - pid_last_d_error);
    if(pid_output > pid_max)pid_output = pid_max;
    else if(pid_output < pid_max * -1)pid_output = pid_max * -1;

    pid_last_d_error = pid_error_temp;
}

void sendThrottle() {
    unsigned char buffer[100];

    //pthread_mutex_lock(&serial_mutex);
    int input = throttleInput;
    // pthread_mutex_unlock(&serial_mutex);
    cout << " | input: " << input;
    int newThrottle = input + pid_output;
    
    if (newThrottle > 1900) newThrottle = 1900;
    //if (newThrottle < 1000) newThrottle = 1000;

    cout << " | Throttle: " << newThrottle << endl;

    buffer[1] = (newThrottle - 1000) & 0xFF;
    buffer[0] = ((newThrottle - 1000) & 0xFF00) >> 8;
    //wiringPiSPIDataRW(SPI_CS, buffer, 2);

    //CLOCK SPEED TEST
    //unsigned long int clockspeed = buffer[1];
    //cout << " | Clock: " << clockspeed << endl;
}



void mainLoop() {
    while(!serialConfigured || !spiConfigured || !authenticated) delay(10);
    // int response = 0;
    // while (response != 0x2222) {
    //     unsigned char buffer[5];
    //     buffer[0] = 0xF9;
    //     buffer[1] = 0xFF;
    //     wiringPiSPIDataRW(SPI_CS, buffer, 2);
    //     response = buffer[0] << 8 | buffer[1];
    // }

    while(run) {
        // cout << throttleInput << endl;
        // fflush(stdout);
        calculateAbsoluteAltitude();
        calculatePID();
        sendThrottle();
    }
}

void *gyroLoop(void *void_ptr) {
    //Switch to flight controller, setup SPI @ 1.5MHz
    SPI_CS = 1;
    // if (wiringPiSPISetup(SPI_CS, 1500000) < 0) {
    //     cout << "SPI Setup Failed: " << strerror(errno) << endl;
    //     fflush(stdout);
    // }
    setupSPI();
    authFlightController();
    while(run) {
        getGyroValues();
    }
    // unsigned char buffer[5];
    // buffer[0] = 0xFF;
    // buffer[1] = 0xF7;
    // wiringPiSPIDataRW(SPI_CS, buffer, 2);
    return NULL;
}

void *serialLoop(void *void_ptr) {
    setupSerial();
    serialFlush(serialFd);
    while(run) {
        handleSerialInterrupt();
        //delay(0.5);
        
    }
    serialClose(serialFd);
    return NULL;
}

void showUsage(string name) {
    cerr << "Usage: " << name << " <option(s)>\n\n"
        << "Options:\n"
        << "\t-h,--help\t\t\tShow this help message\n"
        << "\t-c,--controller-enabled \tRun program to connect with controller\n"
        << "\t-nc,--no-controller \t\tRun program without connecting to controller"
        << endl;
}

//Main Program loop
int main(int argc, char *argv[]) {
    bool controllerConnected = false;
    if (argc > 1) {
        if (string(argv[1]) == "-c" || string(argv[1]) == "--controller-enabled") 
            controllerConnected = true;
        else if (string(argv[1]) == "-nc" || string(argv[1]) == "--no-controller") 
            controllerConnected = false;
        else if (string(argv[1]) == "-h" || string(argv[1]) == "--help") {
            showUsage(argv[0]);
            return 1;
        }
    }
    else {
        showUsage(argv[0]);
        return 1;
    }

    //Setup function calls
    wiringPiSetupGpio();
    setupIOExpander();
    

    pthread_create(&serialThread, NULL, serialLoop, NULL);
    pthread_create(&gyroThread, NULL, gyroLoop, NULL);

    // while(!serialConfigured || !spiConfigured || !authenticated) delay(10);
    // delay(200);
    // cout << "Waiting for gyro calibration..." << endl;
    // fflush(stdout);
    // int start = millis();
    // int repeat = 1;
    // while (gyroRoll != 4) {
    //     repeat = 1;
    //     if (millis() - start > 15000) {
    //         cout << "Gyro not responding, resetting..." << endl;
    //         delay(1000);
    //         authFlightController();
    //         start = 0;
    //         repeat++;
    //     }
    //     else if (repeat > 1) {
    //         shutdown();
    //         return 1;
    //     }
    //     delay(50);
    // }

    // if (controllerConnected) {
    //     cout << "Calibration complete. Arm quadcopter." << endl;
    //     start = millis();
    //     repeat = 1;
    //     while (gyroRoll == 4) {
    //         if (millis() - start > 45000) {
    //             cout << "Gyro not responding, resetting..." << endl;
    //             delay(1000);
    //             authFlightController();
    //             start = 0;
    //             repeat++;
    //         }
    //         if (repeat > 1) {
    //             shutdown();
    //             return 1;
    //         }
    //     }
    // }
    // else {
    //     cout << "Calibration complete. Quadcopter self-arming." << endl;
    // }
    mainLoop();
    delay(2000);
    return 0;
}

void signal_callback_handler(int signum) {
	cout << endl << "Caught signal: " << signum << endl;
    shutdown();

    delay(1000);
	exit(signum);
}