/*
    Copyright (C) Chris Desjardins 2008 - cjd@chrisd.info

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
/*
    Just a quick program to read input from the host servodrive program
    over a socket and pump the data to the servodrive linux device driver.
*/

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/joystick.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include "servosoc.h"

#undef  SERVOCONTROL_DEBUG

class CServoControl
{
public:
    CServoControl();
    ~CServoControl();
    void Drive();

protected:
    void SetupSocket();
    void WriteData(int fdServo);
    void RunDiags(int fdServo);

    int m_nSocket;
    sockaddr_in m_addrMe;
    TServoData m_sServoList[SERVOSOC_MAX_SERVOS];
};

/*
    Init all of the servos that could be controlled
*/
CServoControl::CServoControl()
{
    int i;

    for (i = 0; i < SERVOSOC_MAX_SERVOS; i++)
    {
        m_sServoList[i].m_nJoyValue = 0;
        m_sServoList[i].m_nServoNumber = SERVOSOC_SERVO_INVALID;
    }
    SetupSocket();
}

/*
    Close the socket if it is open
*/
CServoControl::~CServoControl()
{
    if (m_nSocket >= 0)
    {
        close(m_nSocket);
    }
}

/*
    Open the UDP socket to get data from the host side servodrive program
*/
void CServoControl::SetupSocket()
{

    m_nSocket = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_nSocket < 0)
    {
        printf("ERROR: Unable to open socket\n");
    }
    else
    {
        memset(&m_addrMe, 0, sizeof(m_addrMe));
        m_addrMe.sin_family = AF_INET;
        m_addrMe.sin_addr.s_addr = htonl(INADDR_ANY);
        m_addrMe.sin_port = htons(SERVOSOC_PORT);
        if (bind(m_nSocket, (sockaddr*)&m_addrMe, sizeof(m_addrMe)) < 0)
        {
            close(m_nSocket);
            m_nSocket = -1;
        }
    }
}
/*
 * The scale is really from 400-2100, but because 
 * zero is in the middle of the input range (i.e. 0->1250)
 * I adjusted the output range as follows:
 * SERVO_MIN =  ((2100 - 400) / 2) + 400 = 1250
 * SERVO_MAX = SERVO_MIN + 2100
 */
#define SERVO_MAX   (2950.0)   /* Output range */
#define SERVO_MIN   (1250.0)   /* Output range */
#define JOY_MAX     (32767.0)  /* Input range  */
#define JOY_MIN     (-32768.0) /* Input range  */
#define JOYPOS_TO_SERVODELAY(nAxis) (((nAxis / (JOY_MAX - JOY_MIN)) * (SERVO_MAX - SERVO_MIN)) + SERVO_MIN)
 
void CServoControl::WriteData(int fdServo)
{
    TServoData sConvertedList[SERVOSOC_MAX_SERVOS];
    int i;
    for (i = 0; i < SERVOSOC_MAX_SERVOS; i++)
    {
        sConvertedList[i].m_nServoNumber = m_sServoList[i].m_nServoNumber;
        if (m_sServoList[i].m_nServoNumber != SERVOSOC_SERVO_INVALID)
        {
            sConvertedList[i].m_nJoyValue = JOYPOS_TO_SERVODELAY(m_sServoList[i].m_nJoyValue);
        }
    }
    write(fdServo, sConvertedList, sizeof(sConvertedList));
}

/*
    A Quick diags program that just makes the servos go back and forth
*/
void CServoControl::RunDiags(int fdServo)
{
    m_sServoList[0].m_nJoyValue = 32000;
    m_sServoList[0].m_nServoNumber = SERVOSOC_SERVO_A;
    m_sServoList[1].m_nJoyValue = 32000;
    m_sServoList[1].m_nServoNumber = SERVOSOC_SERVO_B;
    WriteData(fdServo);
    sleep(3);
    m_sServoList[0].m_nJoyValue = -32000;
    m_sServoList[0].m_nServoNumber = SERVOSOC_SERVO_A;
    m_sServoList[1].m_nJoyValue = -32000;
    m_sServoList[1].m_nServoNumber = SERVOSOC_SERVO_B;
    WriteData(fdServo);
    sleep(3);
    m_sServoList[0].m_nJoyValue = 0;
    m_sServoList[0].m_nServoNumber = SERVOSOC_SERVO_A;
    m_sServoList[1].m_nJoyValue = 0;
    m_sServoList[1].m_nServoNumber = SERVOSOC_SERVO_B;
    WriteData(fdServo);
    m_sServoList[0].m_nServoNumber = SERVOSOC_SERVO_INVALID;
    m_sServoList[1].m_nServoNumber = SERVOSOC_SERVO_INVALID;
    printf("diags done\n");
}

/*
    This is where the connection is made to the actual servo device driver
    and data is read from the socket and pumped down to the actual servos.
*/
void CServoControl::Drive()
{
    sockaddr_in addrClient;
    socklen_t nAddrLen;
    int fdServo;

    fdServo = open("/dev/servoctrl0", O_WRONLY);
    if (fdServo < 0)
    {
        printf("ERROR: Unable to open servoctrl0\n");
    }
    else
    {
        RunDiags(fdServo);
        while (1)
        {
            if (recvfrom(m_nSocket, m_sServoList, sizeof(m_sServoList), 0, (sockaddr*)&addrClient, &nAddrLen) > 0)
            {
                WriteData(fdServo);
            }
            else
            {
                printf("recvfrom failed %x %s\n", errno, strerror(errno));
            }
        }
        close(fdServo);
    }
}

int main()
{
    CServoControl *pServoControl;
    pServoControl = new CServoControl();
    if (pServoControl == NULL)
    {
        printf("ERROR: Unable to allocate memory for CServoControl\n");
    }
    else
    {
        pServoControl->Drive();
    }
    return 0;
}


