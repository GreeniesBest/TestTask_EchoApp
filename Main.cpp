
#include "Poco/Net/SocketReactor.h"
#include "Poco/Net/SocketAcceptor.h"
#include "Poco/Net/SocketNotification.h"
#include "Poco/Net/StreamSocket.h"
#include "Poco/Net/ServerSocket.h"
#include "Poco/NObserver.h"
#include "Poco/Exception.h"
#include "Poco/Thread.h"
#include "Poco/FIFOBuffer.h"
#include "Poco/Delegate.h"
#include "Poco/Util/ServerApplication.h"
#include "Poco/Util/Option.h"
#include "Poco/Util/OptionSet.h"
#include "Poco/Util/HelpFormatter.h"
#include "Poco/NumberFormatter.h"
#include <iostream>


using Poco::Net::SocketReactor;
using Poco::Net::SocketAcceptor;
using Poco::Net::ReadableNotification;
using Poco::Net::WritableNotification;
using Poco::Net::ShutdownNotification;
using Poco::Net::ServerSocket;
using Poco::Net::StreamSocket;
using Poco::NObserver;
using Poco::AutoPtr;
using Poco::Thread;
using Poco::FIFOBuffer;
using Poco::delegate;
using Poco::Util::ServerApplication;
using Poco::Util::Application;
using Poco::Util::Option;
using Poco::Util::OptionSet;
using Poco::Util::HelpFormatter;


class EchoServiceHandler
{
public:
    EchoServiceHandler( StreamSocket& socket, SocketReactor& reactor ) :
        m_socket( socket ),
        m_reactor( reactor )
    {
        m_readEventOn = true;
        m_writeEventOn = true;

        m_writingString = false;
        
        m_readSize = 0;
        m_readOffset = 0;
        m_stringSize = 0;

        m_writeOffset = 0;

        m_socket.setBlocking( false );
 
        m_reactor.addEventHandler( m_socket, NObserver<EchoServiceHandler, ReadableNotification>( *this, &EchoServiceHandler::OnSocketReadable ) );
        m_reactor.addEventHandler( m_socket, NObserver<EchoServiceHandler, WritableNotification>( *this, &EchoServiceHandler::OnSocketWritable ) );
        m_reactor.addEventHandler( m_socket, NObserver<EchoServiceHandler, ShutdownNotification>( *this, &EchoServiceHandler::OnSocketShutdown ) );

        Application& app = Application::instance();
        app.logger().information( "Connection from " + socket.peerAddress().toString() );

        m_writingString = true;
        snprintf( m_shuffledString, sizeof( m_shuffledString ), "Welcome to the shuffle echo server, type any text and press enter for shuffling\r\n" );
        m_stringSize = strlen( m_shuffledString );
    }

    ~EchoServiceHandler()
    {
        m_reactor.removeEventHandler( m_socket, NObserver<EchoServiceHandler, ReadableNotification>( *this, &EchoServiceHandler::OnSocketReadable ) );
        m_reactor.removeEventHandler( m_socket, NObserver<EchoServiceHandler, WritableNotification>( *this, &EchoServiceHandler::OnSocketWritable ) );
        m_reactor.removeEventHandler( m_socket, NObserver<EchoServiceHandler, ShutdownNotification>( *this, &EchoServiceHandler::OnSocketShutdown ) );

        Application& app = Application::instance();

        try
        {
            app.logger().information( "Disconnecting " + m_socket.peerAddress().toString() );
        }
        catch (...)
        {
        }
    }
 
    bool AddReadEventHandler()
    {
        if ( m_readEventOn == true )
        {
            return false;
        }       

        m_reactor.addEventHandler( m_socket, NObserver<EchoServiceHandler, ReadableNotification>( *this, &EchoServiceHandler::OnSocketReadable ) );
        m_readEventOn = true;
        return true;
    }

    void RemoveReadEventHandler()
    {
        if ( m_readEventOn == false )
        {
            return;
        }

        m_reactor.removeEventHandler( m_socket, NObserver<EchoServiceHandler, ReadableNotification>( *this, &EchoServiceHandler::OnSocketReadable ) );
        m_readEventOn = false;
    }

    bool AddWriteEventHandler()
    {
        if ( m_writeEventOn == true )
        {
            return false;
        }

        m_reactor.addEventHandler( m_socket, NObserver<EchoServiceHandler, WritableNotification>( *this, &EchoServiceHandler::OnSocketWritable ) );
        m_writeEventOn = true;
        return true;
    }

    void RemoveWriteEventHandler()
    {
        if ( m_writeEventOn == false )
        {
            return;
        }

        m_reactor.removeEventHandler( m_socket, NObserver<EchoServiceHandler, WritableNotification>( *this, &EchoServiceHandler::OnSocketWritable ) );
        m_writeEventOn = false;
    }

    bool ReadFromTheSocket()
    {
        bool readOneMoreTime = false;

        do
        {        
            readOneMoreTime = false;

            try
            {
                m_readSize = m_socket.receiveBytes( m_readBuffer, BUFFER_SIZE );

                if ( m_readSize < 0 )
                {
                    // for non blocking socket, readsize < 0 is returned only if there is nothing to read from socket
                    // (i.e recv() returned EAGAIN/EWOULDBLOCK)

                    m_readSize = 0;

                    // try read one more time to cover the case that OS fired "read event" after receiveBytes() call above
                    // and before we called AddReadEventHandler() below
                    readOneMoreTime = AddReadEventHandler();
                }
                else if ( m_readSize == 0 )
                {
                    // socket shutdown
                    delete this;
                    return false;
                }
            }
            catch ( Poco::IOException& exc )       
            {
                switch ( exc.code() )
                {
                    // for non blocking sockets, the IOException( POCO_EWOULDBLOCK ) is not thrown --> so do not handle it here

                    case POCO_EINTR:
                    {
                        readOneMoreTime = true;
                        break;
                    }

                    default:
                    {
                        Application& app = Application::instance();
                        app.logger().log( exc );
                        delete this;
                        return false;
                    }
                }
            }
            catch ( Poco::Exception& exc )
            {
                Application& app = Application::instance();
                app.logger().log( exc );
                delete this;
                return false;
            }
        }
        while ( readOneMoreTime == true );

        return (m_readSize > 0);
    }

    bool WriteStringToTheSocket()
    {
        int sentBytes = 0;
        bool writeOneMoreTime = false;

        do
        {        
            sentBytes = 0;
            writeOneMoreTime = false;

            try
            {
                sentBytes = m_socket.sendBytes( m_shuffledString, m_stringSize - m_writeOffset );
            }
            catch ( Poco::IOException& exc )       
            {
                switch ( exc.code() )
                {
                    case POCO_EWOULDBLOCK:
                    {
                        // try to write one more time to cover the case the OS fired "write event" after sendBytes() call above
                        // and before we called AddWriteEventHandler() below
                        writeOneMoreTime = AddWriteEventHandler();
                        break;
                    }

                    case POCO_EINTR:
                    {
                        writeOneMoreTime = true;
                        break;
                    }

                    default:
                    {
                        Application& app = Application::instance();
                        app.logger().log( exc );
                        delete this;
                        return false;
                    }
                }

            }
            catch ( Poco::Exception& exc )
            {
                Application& app = Application::instance();
                app.logger().log( exc );
                delete this;
                return false;
            }
 
            m_writeOffset += sentBytes;
        }
        while ( ((sentBytes > 0) || (writeOneMoreTime == true)) && (m_writeOffset < m_stringSize) );

        return (m_writeOffset == m_stringSize);
    }

    bool AnalyzeReadData()
    {
        Application& app = Application::instance();

        while ( m_readOffset < m_readSize )
        {       
            while ( (m_readOffset < m_readSize) && (m_readBuffer[m_readOffset] != '\n') )
            {
                if ( m_stringSize < STRING_MAX_SIZE )
                {
                    // copy read data to the string
                    m_string[m_stringSize++] = m_readBuffer[m_readOffset];
                }

                m_readOffset++;
            }

            if ( m_readOffset >= m_readSize )
            {
                // we did not get the \n symbol, continue reading from the socket
                m_readSize = 0;
                m_readOffset = 0;
                return true;
            }

            // skip \n symbol on the input buffer
            m_readOffset++;

            // remove \r symbol from the input string
            if ( (m_stringSize > 0) && (m_string[m_stringSize - 1] == '\r') )
            {
                m_stringSize--;
            }

            // we got the '\n' symbol, shuffle the string, append '\n' to it, start writing to the socket
            for( int i = 0; i < m_stringSize; i++ )
            {
                m_shuffledString[m_stringSize - i - 1] = m_string[i];
            }              

            m_shuffledString[m_stringSize] = '\r';
            m_shuffledString[m_stringSize + 1] = '\n';
            m_stringSize += 2;

            app.logger().information( "got the string \"" + std::string( m_string, m_stringSize - 2 ) + "\" from " + m_socket.peerAddress().toString() );
            app.logger().information( "writing shuffled string \"" + std::string( m_shuffledString, m_stringSize - 2 ) + "\" to " + m_socket.peerAddress().toString() );

            m_writingString = true;
            m_writeOffset = 0;

            if ( WriteStringToTheSocket() == false )
            {
                return false;
            }

            m_writingString = false;
            m_stringSize = 0;
        }
        
        m_readOffset = 0;
        m_readSize = 0;

        return true;
    } 

    void OnSocketReadable( const AutoPtr<ReadableNotification>& pNf )
    {
        if ( m_writingString == true )
        {
            // disable repeated "OnRead" events if we are writing the string for a long time 
            RemoveReadEventHandler();
            return;
        }

        // ReadFromTheSocket() returns true if we read something from the socket
        // AnalyzeReadData() returns true if we need to continue reading from the socket
        while ( (ReadFromTheSocket() == true) && (AnalyzeReadData() == true) );
    }

    void OnSocketWritable( const AutoPtr<WritableNotification>& pNf )
    {
        if ( m_writingString == false )
        {
            // disable repeated "OnWrite" events if we are waiting data from the socket
            RemoveWriteEventHandler();
            return;
        }

        if ( WriteStringToTheSocket() == false )
        {
            return;
        }

        m_writingString = false;
        m_stringSize = 0;

        // AnalyzeReadData() returns true if we need to continue reading from the socket
        // ReadFromTheSocket() returns true if we read something from the socket
        while ( (AnalyzeReadData() == true) && (ReadFromTheSocket() == true) );
    }

    void OnSocketShutdown( const AutoPtr<ShutdownNotification>& pNf )
    {
        delete this;
    }

private:
    enum
    {
        BUFFER_SIZE = 1024,
        STRING_MAX_SIZE = 255
    };
 
    bool m_readEventOn;
    bool m_writeEventOn;

    bool m_writingString;

    char m_readBuffer[BUFFER_SIZE];
    int m_readSize;
    int m_readOffset;
    int m_stringSize;

    char m_string[STRING_MAX_SIZE + 2 + 1];
    char m_shuffledString[STRING_MAX_SIZE + 2 + 1];
    int m_writeOffset;

    StreamSocket m_socket;
    SocketReactor& m_reactor;
};


class EchoServer: public Poco::Util::ServerApplication
{
public:
    EchoServer()
    {
    }

    ~EchoServer()
    {
    }

protected:

    void initialize(Application& self)
    {
        loadConfiguration(); // load default configuration files, if present
        ServerApplication::initialize( self );
    } 

    void displayHelp()
    {
    }

    int main( const std::vector<std::string>& args )
    {
        unsigned short port = 28888;

        // set-up a server socket
        ServerSocket svs( port );
        // set-up a SocketReactor...
        SocketReactor reactor;
        // ... and a SocketAcceptor
        SocketAcceptor<EchoServiceHandler> acceptor( svs, reactor );
        // run the reactor in its own thread so that we can wait for
        // a termination request
        Thread thread;

        thread.start( reactor );

        // wait for CTRL-C or kill
        waitForTerminationRequest();

        // Stop the SocketReactor
        reactor.stop();
        thread.join();

        return Application::EXIT_OK;
    }
};


int main( int argc, char** argv )
{
    EchoServer app;
    return app.run( argc, argv );
} 


