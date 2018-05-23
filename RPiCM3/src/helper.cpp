#include <helper.h>

bool controllerConnected = false;

std::string camera;
std::string receiver;

Radio<Serial> radio;
FlightController fc;
Barometer baro;
std::thread serialThread;

bool keyLoopActive;
bool shuttingDown = false;
bool doneShuttingDown = false;
bool startCli = false;

/// @brief Displays message showing how to type options in command line.
/// @param name Name of program, i.e. First string of argv[].
void showUsage(std::string name) {
    std::cerr << "Usage: " << name << " <option(s)>\n"
        << "**NOTE: Must be run with root privileges (sudo)\n\n"
        << "Options:\n"
        << "\t-h,--help\t\t\tShow this help message\n"
        << "\t-c,--controller-enabled \tRun program to connect with controller\n"
        << "\t-nc,--no-controller \t\tRun program without connecting to controller\n"
        << "\t-c,--camera [camera (ex. /dev/video0)] \tStreaming camera\n"
        << "\t-r,--receiver [IP Address] \tStreaming receiver IP Address\n"
        << "\t-s,--stream \t\t\tEnable streaming\n\n";
}

/// @brief Terminal signal handler (for ending program via terminal).
void signal_callback_handler(int signum) {
    if (!shuttingDown) shutdown();
    while (!doneShuttingDown);
    std::cout << "\nProgram End\n";
    delay(1000);
	exit(0);
}

/// @brief Main program loop after pre-flight checks.
void mainLoop() {
    std::cout << "Waiting for configuration...\n";
    // Waits until Flight Controller is ready, after SPI port is opened,
    // and STM32F446 is authenticated.
    while(!fc.isSPIConfigured() || !fc.isAuthenticated()) delay(10);
    std::cout << "Starting main loop\n";

    // Checks if program is running in gyro testing mode.
    if (fc.isTestGyroActive()) {
        while(fc.isRunning()) {
            std::cout << "Pitch: " << fc.getDronePosition().pitch 
                << "\t| Roll: " << fc.getDronePosition().roll << "\n";
            delay(20);
        }
    }

    // If the drone is not being controlled by handheld controller.
    if (!controllerConnected) {
        int heartbeatTimer, baroTimer = millis();
        int loopTimer = micros();
        float currentAltitude = 0;
        radio.setupSerial("/dev/serial0", 115200);
        
        printf("Calibrating barometer...\n");
        baro.setup();
        //radio.startReceiveLoop();
        while(fc.isRunning()) {
            // //Every second, send heartbeat to controller
            // if (millis() - heartbeatTimer > 1000) {
            //     radioBuffer msg = radio.sendHeartbeat(0,3); //Heartbeat in PREFLIGHT mode and STANDBY state
            //     radio.write(msg.buf, msg.len);
            //     heartbeatTimer = millis();
            // }

            //Every 260ms, get pressure altitude, if barometer is calibrated
            if (baro.isCalibrated() && (millis() - baroTimer > BARO_DELAY)) {
                currentAltitude = baro.getPressureAltitude();
                std::cout << "BARO: Altitude: " << currentAltitude << "m\n";
                baro.takeReading();
                baroTimer = millis();
            }

            radio.customReceiveByte(radio.readChar());
            // radio.mavlinkReceiveByte(radio.readChar());
            // channels pwmInputs = radio.getRCChannels();
            // std::cout << "Pitch: " << pwmInputs.pitchPWM
            //     << "\nRoll: " << pwmInputs.rollPWM
            //     << "\nYaw: " << pwmInputs.yawPWM
            //     << "\nthrottle: " << pwmInputs.throttlePWM
            //     << "\ntime: " << micros() - loopTimer << "\n----\n";

            //Calculate new PID compensated throttle
            //uint16_t newThrottle = fc.calculateThrottlePID(pwmInputs.throttlePWM, currentAltitude);
            
            loopTimer = micros();
        }
    }
    else {
        while(fc.isRunning()) {
            //delay(100);
            //calculatePID();
        }
    }
}

/// @brief Shutting down threads and closing ports.
void shutdown() {
    shuttingDown = true;

    std::cout << "\nClosing Threads and Ports...\n\n";

    // Stop threads
    fc.stopFlight();
    delay(100);
    radio.stopReceiveLoop();
    baro.close();

    std::cout << "Closing I2C, UART, SPI, TCP Socket...\n";

    // Close all interfaces on global instances so that we
    // can close the ports before terminating pigpio
    fc.closeSPI();
    radio.closeSerial();

    std::cout << "\nResetting Flight Controller...\n\n";
    
    // Reset command to STM32
    fc.requestService(FlightController::Service::RESET);

    // Terminates pigpio library
    gpioTerminate();

    // Signals to main() that it can return now
    doneShuttingDown = true;
}

/// @brief Filtering command line options.
/// @param _argc Pass in argc from main() here.
/// @param _argv Pass in argv from main() here.
void filterCommandLineOptions(int _argc, char *_argv[]) {
    if (_argc > 1) {
        for (int i = 1; i < _argc; i++) {
            if (std::string (_argv[i]) == "-c" || std::string (_argv[i]) == "--controller-enabled") 
                controllerConnected = true;
            else if (std::string (_argv[i]) == "-nc" || std::string (_argv[i]) == "--no-controller") 
                controllerConnected = false;
            if (std::string (_argv[i]) == "-tg" || std::string (_argv[i]) == "--test-gyro")
                fc.setTestGyro(true);
            if (std::string (_argv[i]) == "-mt" || std::string (_argv[i]) == "--motor-test") {
                std::cout << "\n\t\t!!!! TESTING MOTORS BEFORE ARM !!!!\n\n";
                fc.setMotorTest(true);
            }
            if (std::string (_argv[i]) == "-nm" || std::string (_argv[i]) == "--no-motors")
                fc.setNoMotors(true);
            if (std::string (_argv[i]) == "-c" || std::string (_argv[i]) == "--camera") {
                camera = std::string (_argv[i+1]);
            }
            if (std::string (_argv[i]) == "-r" || std::string (_argv[i]) == "--receiver") {
                receiver = std::string (_argv[i+1]);
            }
            if (std::string (_argv[i]) == "-h" || std::string (_argv[i]) == "--help") {
                showUsage (_argv[0]);
                exit(0);
            }
        } 
    }
    // If no options are typed, show how to type them.
    else {
        showUsage (_argv[0]);
        exit(0);
    }
}

/// @brief Waiting for gyro calibration on STM32F446 to finish.
void waitForGyroCalibration() {
    while(!fc.isSPIConfigured() || !fc.isAuthenticated()) delay(10);
    delay(200);
    std::cout << "Waiting for gyro calibration...\n";
    fflush(stdout);
    int start = millis();
    int repeat = 1;
    while (fc.getReceivedData() != GYRO_CAL) {
        if (millis() - start > 10000) {
            std::cout << "Gyro not responding, resetting...\n";
            delay(1000);
            fc.requestService(FlightController::Service::AUTH);
            while(!fc.isAuthenticated());
            start = millis();
            repeat += 1;
        }
        else if (repeat > 1) {
            std::cout << "Gyro still not responding, shutting down...\n";
            shutdown();
            exit(1);
        }
        delay(50);
    }
    std::cout << "Calibration complete\n";
}