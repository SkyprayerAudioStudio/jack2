/*
Copyright (C) 2008-2011 Romain Moret at Grame

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#include "JackMidiPort.h"
#include "JackTools.h"
#include "JackPlatformPlug.h"
#include "types.h"
#include "transport.h"
#ifndef WIN32
#include <netinet/in.h>
#endif
#include <cmath>

using namespace std;

#ifndef htonll
#ifdef __BIG_ENDIAN__
#define htonll(x) (x)
#define ntohll(x) (x)
#else
#define htonll(x) ((((uint64_t)htonl(x)) << 32) + htonl(x >> 32))
#define ntohll(x) ((((uint64_t)ntohl(x)) << 32) + ntohl(x >> 32))
#endif
#endif

#define MASTER_PROTOCOL 4
#define SLAVE_PROTOCOL 4

#define NET_PACKET_ERROR -2

#define OPTIMIZED_PROTOCOL

#define HEADER_SIZE (sizeof(packet_header_t))
#define PACKET_AVAILABLE_SIZE(params) ((params)->fMtu - sizeof(packet_header_t))

namespace Jack
{
    typedef struct _session_params session_params_t;
    typedef struct _packet_header packet_header_t;
    typedef struct _net_transport_data net_transport_data_t;
    typedef struct sockaddr socket_address_t;
    typedef struct in_addr address_t;
    typedef jack_default_audio_sample_t sample_t;

    enum JackNetEncoder {

        JackFloatEncoder = 0,
        JackIntEncoder = 1,
        JackCeltEncoder = 2,
    };

//session params ******************************************************************************

    /**
    \brief This structure containes master/slave connection parameters, it's used to setup the whole system

    We have :
        - some info like version, type and packet id
        - names
        - network parameters (hostnames and mtu)
        - nunber of audio and midi channels
        - sample rate and buffersize
        - number of audio frames in one network packet (depends on the channel number)
        - is the NetDriver in Sync or ASync mode ?
        - is the NetDriver linked with the master's transport

    Data encoding : headers (session_params and packet_header) are encoded using HTN kind of functions but float data
    are kept in LITTLE_ENDIAN format (to avoid 2 conversions in the more common LITTLE_ENDIAN <==> LITTLE_ENDIAN connection case).
    */

    struct _session_params
    {
        char fPacketType[7];                //packet type ('param')
        char fProtocolVersion;              //version
        uint32_t fPacketID;                 //indicates the packet type
        char fName[JACK_CLIENT_NAME_SIZE];  //slave's name
        char fMasterNetName[256];           //master hostname (network)
        char fSlaveNetName[256];            //slave hostname (network)
        uint32_t fMtu;                      //connection mtu
        uint32_t fID;                       //slave's ID
        uint32_t fTransportSync;            //is the transport synced ?
        int32_t fSendAudioChannels;         //number of master->slave channels
        int32_t fReturnAudioChannels;       //number of slave->master channels
        int32_t fSendMidiChannels;          //number of master->slave midi channels
        int32_t fReturnMidiChannels;        //number of slave->master midi channels
        uint32_t fSampleRate;               //session sample rate
        uint32_t fPeriodSize;               //period size
        uint32_t fSampleEncoder;            //samples encoder
        uint32_t fKBps;                     //KB per second for CELT encoder
        uint32_t fSlaveSyncMode;            //is the slave in sync mode ?
        uint32_t fNetworkLatency;           //network latency
    };

//net status **********************************************************************************

    /**
    \Brief This enum groups network error by type
    */

    enum  _net_status
    {
        NET_SOCKET_ERROR = 0,
        NET_CONNECT_ERROR,
        NET_ERROR,
        NET_SEND_ERROR,
        NET_RECV_ERROR,
        NET_CONNECTED,
        NET_ROLLING
    };

    typedef enum _net_status net_status_t;

//sync packet type ****************************************************************************

    /**
    \Brief This enum indicates the type of a sync packet (used in the initialization phase)
    */

    enum _sync_packet_type
    {
        INVALID = 0,        //...
        SLAVE_AVAILABLE,    //a slave is available
        SLAVE_SETUP,        //slave configuration
        START_MASTER,       //slave is ready, start master
        START_SLAVE,        //master is ready, activate slave
        KILL_MASTER         //master must stop
    };

    typedef enum _sync_packet_type sync_packet_type_t;


//packet header *******************************************************************************

    /**
    \Brief This structure is a complete header

    A header indicates :
        - it is a header
        - the type of data the packet contains (sync, midi or audio)
        - the path of the packet (send -master->slave- or return -slave->master-)
        - the unique ID of the slave
        - the sample's bitdepth (unused for now)
        - the size of the midi data contains in the packet (indicates how much midi data will be sent)
        - the number of midi packet(s) : more than one is very unusual, it depends on the midi load
        - the ID of the current cycle (used to check missing packets)
        - the ID of the packet subcycle (for audio data)
        - a flag indicating this packet is the last of the cycle (for sync robustness, it's better to process this way)
        - a flag indicating if, in async mode, the previous graph was not finished or not
        - padding to fill 64 bytes

    */

    struct _packet_header
    {
        char fPacketType[7];        //packet type ('headr')
        char fDataType;             //a for audio, m for midi and s for sync
        char fDataStream;           //s for send, r for return
        uint32_t fID;               //unique ID of the slave
        uint32_t fNumPacket;        //number of data packets of the cycle
        uint32_t fPacketSize;       //packet size in bytes
        uint32_t fActivePorts;      //number of active ports
        uint32_t fCycle;            //process cycle counter
        uint32_t fSubCycle;         //midi/audio subcycle counter
        uint32_t fIsLastPckt;       //is it the last packet of a given cycle ('y' or 'n')
    };

//net timebase master

    /**
    \Brief This enum describes timebase master's type
    */

    enum _net_timebase_master
    {
        NO_CHANGE = 0,
        RELEASE_TIMEBASEMASTER = 1,
        TIMEBASEMASTER = 2,
        CONDITIONAL_TIMEBASEMASTER = 3
    };

    typedef enum _net_timebase_master net_timebase_master_t;


//transport data ******************************************************************************

    /**
    \Brief This structure contains transport data to be sent over the network
    */

    struct _net_transport_data
    {
        uint32_t fNewState;             //is it a state change
        uint32_t fTimebaseMaster;       //is there a new timebase master
        int32_t fState;                 //current cycle state
        jack_position_t fPosition;      //current cycle position
    };

 //midi data ***********************************************************************************

    /**
    \Brief Midi buffer and operations class

    This class is a toolset to manipulate Midi buffers.
    A JackMidiBuffer has a fixed size, which is the same than an audio buffer size.
    An intermediate fixed size buffer allows to uninterleave midi data (from jack ports).
    But for a big majority of the process cycles, this buffer is filled less than 1%,
    Sending over a network 99% of useless data seems completely unappropriate.
    The idea is to count effective midi data, and then send the smallest packet we can.
    To do it, we use an intermediate buffer.
    We have two methods to convert data from jack ports to intermediate buffer,
    And two others to convert this intermediate buffer to a network buffer (header + payload data)

    */

    class SERVER_EXPORT NetMidiBuffer
    {
        private:

            int fNPorts;
            size_t fMaxBufsize;
            int fMaxPcktSize;
            char* fBuffer;
            char* fNetBuffer;
            JackMidiBuffer** fPortBuffer;

            size_t fCycleSize;  // needed size in bytes ofr an entire cycle

        public:

            NetMidiBuffer(session_params_t* params, uint32_t nports, char* net_buffer);
            ~NetMidiBuffer();

            void Reset();

            // needed size in bytes for an entire cycle
            size_t GetCycleSize();

            int GetNumPackets(int data_sizen, int max_size);

            //utility
            void DisplayEvents();

            //jack<->buffer
            int RenderFromJackPorts();
            void RenderToJackPorts();

            //network<->buffer
            void RenderFromNetwork(int sub_cycle, size_t copy_size);
            int RenderToNetwork(int sub_cycle, size_t total_size);

            void SetBuffer(int index, JackMidiBuffer* buffer);
            JackMidiBuffer* GetBuffer(int index);
    };

// audio data *********************************************************************************

    class SERVER_EXPORT NetAudioBuffer
    {

        protected:

            int fNPorts;
            int fLastSubCycle;
            char* fNetBuffer;
            sample_t** fPortBuffer;

        public:

            NetAudioBuffer()
            {}
            virtual ~NetAudioBuffer()
            {}

            // needed syze in bytes ofr an entire cycle
            virtual size_t GetCycleSize() = 0;

            // cycle duration in sec
            virtual float GetCycleDuration() = 0;

            virtual int GetNumPackets() = 0;

            //jack<->buffer
            virtual void RenderFromJackPorts() = 0;
            virtual void RenderToJackPorts() = 0;

            //network<->buffer
            virtual int RenderFromNetwork(int cycle, int sub_cycle, size_t copy_size, uint32_t port_num) = 0;
            virtual void ActivePortsFromNetwork(char* net_buffer, uint32_t port_num) {}

            virtual int RenderToNetwork(int sub_cycle, uint32_t& port_num) = 0;
            virtual void ActivePortsToNetwork(char* net_buffer, uint32_t& port_num) {}

            virtual void SetBuffer(int index, sample_t* buffer) = 0;
            virtual sample_t* GetBuffer(int index) = 0;


            virtual void ActivePortsToNetwork(uint32_t& port_num);
            virtual void ActivePortsFromNetwork(uint32_t port_num);

   };

    /**
    \Brief Audio buffer and operations class

    This class is a toolset to manipulate audio buffers.
    The manipulation of audio buffers is similar to midi buffer, except those buffers have fixed size.
    The interleaving/uninterleaving operations are simplier here because audio buffers have fixed size,
    So there is no need of an intermediate buffer as in NetMidiBuffer.

    */

    struct JackPortList {

        // "[---Header---|--audio data--|--audio data--]..."

        jack_nframes_t fPeriodSize;
        jack_nframes_t fSubPeriodSize;
        size_t fSubPeriodBytesSize;
        sample_t** fPortBuffer;
        int fPacketSize;
        int fNPorts;
        size_t fCycleSize;      // needed size in bytes for an entire cycle
        float fCycleDuration;   // in sec

        int fLastSubCycle;

        JackPortList(session_params_t* params, uint32_t nports)
        {
            fNPorts = nports;
            fPeriodSize = params->fPeriodSize;
            fPacketSize = params->fMtu - sizeof(packet_header_t);

            if (params->fSendAudioChannels == 0 && params->fReturnAudioChannels == 0) {
                fSubPeriodSize = params->fPeriodSize;
            } else {
                jack_nframes_t period = (int) powf(2.f,(int)(log(float(fPacketSize)
                                                   / (max(params->fReturnAudioChannels, params->fSendAudioChannels) * sizeof(sample_t))) / log(2.)));
                fSubPeriodSize = (period > fPeriodSize) ? fPeriodSize : period;
            }

            fSubPeriodBytesSize = fSubPeriodSize * sizeof(sample_t);

            fPortBuffer = new sample_t* [fNPorts];
            for (int port_index = 0; port_index < fNPorts; port_index++) {
                fPortBuffer[port_index] = NULL;
            }

            fCycleDuration = float(fSubPeriodSize) / float(params->fSampleRate);
            fCycleSize = params->fMtu * (fPeriodSize / fSubPeriodSize);

            fLastSubCycle = -1;
        }

        virtual int GetNumPackets()
        {
            /*
            jack_log("GetNumPackets packet = %d  fPeriodSize = %d fSubPeriodSize = %d fSubPeriodBytesSize = %d",
                fPeriodSize / fSubPeriodSize, fPeriodSize, fSubPeriodSize, fSubPeriodBytesSize);
            */
            return fPeriodSize / fSubPeriodSize;
        }

        JackPortList()
        {
            fNPorts = 0;
            fPeriodSize = 0;
            fSubPeriodSize = 0;
            fSubPeriodBytesSize = 0;
            fPortBuffer = 0;
        }

        virtual ~JackPortList()
        {
            delete [] fPortBuffer;
        }

        void SetBuffer(int index, sample_t* buffer)
        {
            fPortBuffer[index] = buffer;
        }

        sample_t* GetBuffer(int index)
        {
            return fPortBuffer[index];
        }

        void Copy(sample_t** buffers)
        {
            for (int port_index = 0; port_index < fNPorts; port_index++) {
                memcpy(buffers[port_index], fPortBuffer[port_index], fPeriodSize * sizeof(float));
            }
        }

        // needed syze in bytes for an entire cycle
        size_t GetCycleSize()
        {
            return fCycleSize;
        }

        // cycle duration in sec
        float GetCycleDuration()
        {
            return fCycleDuration;
        }

    #ifdef __BIG_ENDIAN__

        static inline float SwapFloat(float f)
        {
              union
              {
                float f;
                unsigned char b[4];
              } dat1, dat2;

              dat1.f = f;
              dat2.b[0] = dat1.b[3];
              dat2.b[1] = dat1.b[2];
              dat2.b[2] = dat1.b[1];
              dat2.b[3] = dat1.b[0];
              return dat2.f;
        }

        virtual void RenderFromJackPorts()
        {}

        virtual void RenderToJackPorts()
        {}

        //network<->buffer
        virtual int RenderFromNetwork(char* net_buffer, int cycle, int sub_cycle, size_t copy_size, uint32_t port_num)
        {
            int res = 0;

            for (int port_index = 0; port_index < fNPorts; port_index++) {
                float* src = (float*)(net_buffer + port_index * fSubPeriodBytesSize);
                float* dst = (float*)(fPortBuffer[port_index] + sub_cycle * fSubPeriodSize);
                for (unsigned int sample = 0; sample < fSubPeriodBytesSize / sizeof(float); sample++) {
                    dst[sample] = SwapFloat(src[sample]);
                }
            }
            if (sub_cycle != fLastSubCycle + 1) {
                jack_error("Packet(s) missing from... %d %d", fLastSubCycle, sub_cycle);
                res = NET_PACKET_ERROR;
            }

            fLastSubCycle = sub_cycle;
            return res;
        }

        virtual int RenderToNetwork(char* net_buffer, int sub_cycle, uint32_t& port_num)
        {
            for (int port_index = 0; port_index < fNPorts; port_index++) {
                float* src = (float*)(fPortBuffer[port_index] + sub_cycle * fSubPeriodSize);
                float* dst = (float*)(net_buffer + port_index * fSubPeriodBytesSize);
                for (unsigned int sample = 0; sample < fSubPeriodBytesSize / sizeof(float); sample++) {
                    dst[sample] = SwapFloat(src[sample]);
                }
            }

            port_num = fNPorts;
            return fNPorts * fSubPeriodBytesSize;
        }

    #else

        virtual void RenderFromJackPorts()
        {}

        virtual void RenderToJackPorts()
        {
            // reset for next cycle
            fLastSubCycle = -1;
        }

        //network<->buffer
        virtual int RenderFromNetwork(char* net_buffer, int cycle, int sub_cycle, size_t copy_size, uint32_t port_num)
        {
            int res = 0;

            for (int port_index = 0; port_index < fNPorts; port_index++) {
                memcpy(fPortBuffer[port_index] + sub_cycle * fSubPeriodSize, net_buffer + port_index * fSubPeriodBytesSize, fSubPeriodBytesSize);
            }
            if (sub_cycle != fLastSubCycle + 1) {
                jack_error("Packet(s) missing from... %d %d", fLastSubCycle, sub_cycle);
                res = NET_PACKET_ERROR;
            }

            fLastSubCycle = sub_cycle;
            return res;
        }

        virtual int RenderToNetwork(char* net_buffer, int sub_cycle, uint32_t& port_num)
        {
            for (int port_index = 0; port_index < fNPorts; port_index++) {
                memcpy(net_buffer + port_index * fSubPeriodBytesSize, fPortBuffer[port_index] + sub_cycle * fSubPeriodSize, fSubPeriodBytesSize);
            }
            port_num = fNPorts;
            return fNPorts * fSubPeriodBytesSize;
        }

    #endif

        virtual void ActivePortsFromNetwork(char* net_buffer, uint32_t port_num)
        {}

        virtual void ActivePortsToNetwork(char* net_buffer, uint32_t& port_num)
        {
            // Default behaviour: all ports are active
            port_num = fNPorts;
        }

    };

    struct JackOptimizedPortList : JackPortList {

        // Consuming port list is transmitted in the Sync packed
        // "[---Header---|--active_port_num (uint32_t) ---audio data--|--active_port_num (uint32_t) ---audio data--]..."

        JackOptimizedPortList(session_params_t* params, uint32_t nports)
            :JackPortList(params, nports)
        {}

        virtual ~JackOptimizedPortList()
        {}

        int GetNumPackets()
        {
            // Count active ports
            int active_ports = 0;
            for (int port_index = 0; port_index < fNPorts; port_index++) {
                if (fPortBuffer[port_index]) active_ports++;
            }

            if (active_ports == 0) {
                fSubPeriodSize = fPeriodSize;
            } else {
                jack_nframes_t period = (int) powf(2.f, (int)(log(float(fPacketSize) / (active_ports * sizeof(sample_t))) / log(2.)));
                fSubPeriodSize = (period > fPeriodSize) ? fPeriodSize : period;
            }

            fSubPeriodBytesSize = fSubPeriodSize * sizeof(sample_t) + sizeof(uint32_t); // The port number in coded on 4 bytes

            /*
            jack_log("GetNumPackets packet = %d  fPeriodSize = %d fSubPeriodSize = %d fSubPeriodBytesSize = %d",
                fPeriodSize / fSubPeriodSize, fPeriodSize, fSubPeriodSize, fSubPeriodBytesSize);
            */
            return fPeriodSize / fSubPeriodSize; // At least one packet
        }

    #ifdef __BIG_ENDIAN__

        // TODO

    #else

        //network<->buffer
        virtual int RenderFromNetwork(char* net_buffer, int cycle, int sub_cycle, size_t copy_size, uint32_t port_num)
        {
            int res = 0;

            // Cleanup all JACK ports at the beginning of the cycle
            if (sub_cycle == 0) {
                for (int port_index = 0; port_index < fNPorts; port_index++) {
                    if (fPortBuffer[port_index]) {
                        memset(fPortBuffer[port_index], 0, fPeriodSize * sizeof(sample_t));
                    }
                }
            }

            if (port_num > 0)  {

                /// Setup rendering parameters
                int sub_period_size, sub_period_bytes_size;
                if (port_num == 0) {
                    sub_period_size = fPeriodSize;
                } else {
                    jack_nframes_t period = (int) powf(2.f, (int)(log(float(fPacketSize) / (port_num * sizeof(sample_t))) / log(2.)));
                    sub_period_size = (period > fPeriodSize) ? fPeriodSize : period;
                }
                sub_period_bytes_size = sub_period_size * sizeof(sample_t) + sizeof(uint32_t); // The port number in coded on 4 bytes

                for (uint32_t port_index = 0; port_index < port_num; port_index++) {
                    // Only copy to active ports : read the active port number then audio data
                    uint32_t* active_port_address = (uint32_t*)(net_buffer + port_index * sub_period_bytes_size);
                    uint32_t active_port = (uint32_t)(*active_port_address);
                    if (fPortBuffer[port_index]) {
                        memcpy(fPortBuffer[active_port] + sub_cycle * sub_period_size, (char*)(active_port_address + 1), sub_period_bytes_size - sizeof(uint32_t));
                    }
                }

                if (sub_cycle != fLastSubCycle + 1) {
                    jack_error("Packet(s) missing from... %d %d", fLastSubCycle, sub_cycle);
                    res = NET_PACKET_ERROR;
                }

                fLastSubCycle = sub_cycle;
            }

            return res;
        }

        virtual int RenderToNetwork(char* net_buffer,int sub_cycle, uint32_t& port_num)
        {
            // Init active port count
            port_num = 0;

            for (int port_index = 0; port_index < fNPorts; port_index++) {
                // Only copy from active ports : write the active port number then audio data
                if (fPortBuffer[port_index]) {
                    uint32_t* active_port_address = (uint32_t*)(net_buffer + port_num * fSubPeriodBytesSize);
                    *active_port_address = port_index;
                    memcpy((char*)(active_port_address + 1), fPortBuffer[port_index] + sub_cycle * fSubPeriodSize, fSubPeriodBytesSize - sizeof(uint32_t));
                    port_num++;
                }
            }

            return port_num * fSubPeriodBytesSize;
        }

    #endif

        virtual void ActivePortsToNetwork(char* net_buffer, uint32_t& port_num)
        {
            // Init active port count
            port_num = 0;
            short* active_port_address = (short*)net_buffer;

            for (int port_index = 0; port_index < fNPorts; port_index++) {
                // Write the active port number
                if (fPortBuffer[port_index]) {
                    *active_port_address = port_index;
                    active_port_address++;
                    port_num++;
                    assert(port_num < 512);
                }
            }
        }

        virtual void ActivePortsFromNetwork(char* net_buffer, uint32_t port_num)
        {
            short* active_port_address = (short*)net_buffer;

            for (int port_index = 0; port_index < fNPorts; port_index++) {
                fPortBuffer[port_index] = NULL;
            }

            for (uint port_index = 0; port_index < port_num; port_index++) {
                // Use -1 when port is actually connected on other side
                if (*active_port_address >= 0 && *active_port_address < fNPorts) {
                    fPortBuffer[*active_port_address] = (sample_t*)-1;
                } else {
                    jack_error("ActivePortsFromNetwork: incorrect port = %d", *active_port_address);
                }
                active_port_address++;
            }
        }

    };

    class SERVER_EXPORT NetFloatAudioBuffer : public NetAudioBuffer
    {
        private:

        #ifdef OPTIMIZED_PROTOCOL
            JackOptimizedPortList fPortBuffer1;
        #else
            JackPortList fPortBuffer1;
        #endif

        public:

            NetFloatAudioBuffer(session_params_t* params, uint32_t nports, char* net_buffer);
            virtual ~NetFloatAudioBuffer();

            // needed size in bytes for an entire cycle
            size_t GetCycleSize();

            // cycle duration in sec
            float GetCycleDuration()
            {
                return fPortBuffer1.GetCycleDuration();
            }

            int GetNumPackets()
            {
                return fPortBuffer1.GetNumPackets();
            }

            //jack<->buffer
            void RenderFromJackPorts();
            void RenderToJackPorts();

            void SetBuffer(int index, sample_t* buffer);
            sample_t* GetBuffer(int index);

            //network<->buffer
            int RenderFromNetwork(int cycle, int sub_cycle, size_t copy_size, uint32_t port_num);
            void ActivePortsFromNetwork(char* net_buffer, uint32_t port_num);

            int RenderToNetwork(int sub_cycle, uint32_t&  ort_num);
            void ActivePortsToNetwork(char* net_buffer, uint32_t& port_num);
    };

    class SERVER_EXPORT NetFloatAudioBuffer1 : public NetAudioBuffer
    {

        private:

            jack_nframes_t fPeriodSize;
            jack_nframes_t fSubPeriodSize;
            size_t fSubPeriodBytesSize;
            //sample_t** fPortBuffer;
            int fPacketSize;
            //int fNPorts;
            size_t fCycleSize;      // needed size in bytes for an entire cycle
            float fCycleDuration;   // in sec

            //int fLastSubCycle;

        public:

            NetFloatAudioBuffer1(session_params_t* params, uint32_t nports, char* net_buffer);
            virtual ~NetFloatAudioBuffer1();

            // needed size in bytes for an entire cycle
            size_t GetCycleSize();

            // cycle duration in sec
            float GetCycleDuration();

            int GetNumPackets();

            void SetBuffer(int index, sample_t* buffer);
            sample_t* GetBuffer(int index);

            virtual void RenderFromJackPorts()
            {}

            virtual void RenderToJackPorts()
            {
                // reset for next cycle
                fLastSubCycle = -1;
            }

            //jack<->buffer
            int RenderFromNetwork(int cycle, int sub_cycle, size_t copy_size, uint32_t port_num);
            int RenderToNetwork(int sub_cycle, uint32_t& port_num);

            /*
            void ActivePortsToNetwork(uint32_t& port_num);
            void ActivePortsFromNetwork(uint32_t port_num);
            */


    };

#if HAVE_CELT

#include <celt/celt.h>

    class SERVER_EXPORT NetCeltAudioBuffer : public NetAudioBuffer
    {
        private:

            CELTMode** fCeltMode;
            CELTEncoder** fCeltEncoder;
            CELTDecoder** fCeltDecoder;

            int fCompressedSizeByte;
            jack_nframes_t fPeriodSize;

            int fNumPackets;
            float fCycleDuration;   // in sec
            size_t fCycleSize;      // needed size in bytes for an entire cycle

            size_t fSubPeriodBytesSize;
            size_t fLastSubPeriodBytesSize;

            //sample_t** fPortBuffer;
            unsigned char** fCompressedBuffer;

            void FreeCelt();

        public:

            NetCeltAudioBuffer(session_params_t* params, uint32_t nports, char* net_buffer, int kbps);
            virtual ~NetCeltAudioBuffer();

            // needed size in bytes for an entire cycle
            size_t GetCycleSize();

             // cycle duration in sec
            float GetCycleDuration();
            int GetNumPackets();

            void SetBuffer(int index, sample_t* buffer);
            sample_t* GetBuffer(int index);

            //jack<->buffer
            void RenderFromJackPorts();
            void RenderToJackPorts();

            //network<->buffer
            int RenderFromNetwork(int cycle, int sub_cycle, size_t copy_size, uint32_t port_num);
            int RenderToNetwork(int sub_cycle, uint32_t&  port_num);
    };

#endif

    class SERVER_EXPORT NetIntAudioBuffer : public NetAudioBuffer
    {
        private:

            int fCompressedSizeByte;
            jack_nframes_t fPeriodSize;

            int fNumPackets;

            float fCycleDuration;   // in sec
            size_t fCycleSize;      // needed size in bytes for an entire cycle

            size_t fSubPeriodSize;
            size_t fSubPeriodBytesSize;

            size_t fLastSubPeriodSize;
            size_t fLastSubPeriodBytesSize;

            //sample_t** fPortBuffer;
            short** fIntBuffer;

        public:

            NetIntAudioBuffer(session_params_t* params, uint32_t nports, char* net_buffer);
            virtual ~NetIntAudioBuffer();

            // needed size in bytes for an entire cycle
            size_t GetCycleSize();

             // cycle duration in sec
            float GetCycleDuration();
            int GetNumPackets();

            void SetBuffer(int index, sample_t* buffer);
            sample_t* GetBuffer(int index);

            //jack<->buffer
            void RenderFromJackPorts();
            void RenderToJackPorts();

            //network<->buffer
            int RenderFromNetwork(int cycle, int sub_cycle, size_t copy_size, uint32_t port_num);
            int RenderToNetwork(int sub_cycle, uint32_t& port_num);
    };

    //utility *************************************************************************************

    //socket API management
    SERVER_EXPORT int SocketAPIInit();
    SERVER_EXPORT int SocketAPIEnd();
    //n<-->h functions
    SERVER_EXPORT void SessionParamsHToN(session_params_t* src_params, session_params_t* dst_params);
    SERVER_EXPORT void SessionParamsNToH(session_params_t* src_params, session_params_t* dst_params);
    SERVER_EXPORT void PacketHeaderHToN(packet_header_t* src_header, packet_header_t* dst_header);
    SERVER_EXPORT void PacketHeaderNToH(packet_header_t* src_header, packet_header_t* dst_header);
    SERVER_EXPORT void MidiBufferHToN(JackMidiBuffer* src_buffer, JackMidiBuffer* dst_buffer);
    SERVER_EXPORT void MidiBufferNToH(JackMidiBuffer* src_buffer, JackMidiBuffer* dst_buffer);
    SERVER_EXPORT void TransportDataHToN(net_transport_data_t* src_params, net_transport_data_t* dst_params);
    SERVER_EXPORT void TransportDataNToH(net_transport_data_t* src_params, net_transport_data_t* dst_params);
    //display session parameters
    SERVER_EXPORT void SessionParamsDisplay(session_params_t* params);
    //display packet header
    SERVER_EXPORT void PacketHeaderDisplay(packet_header_t* header);
    //get the packet type from a sesion parameters
    SERVER_EXPORT sync_packet_type_t GetPacketType(session_params_t* params);
    //set the packet type in a session parameters
    SERVER_EXPORT int SetPacketType(session_params_t* params, sync_packet_type_t packet_type);
    //transport utility
    SERVER_EXPORT const char* GetTransportState(int transport_state);
    SERVER_EXPORT void NetTransportDataDisplay(net_transport_data_t* data);
}
