// Copyright 2015, Christopher J. Foster and the other displaz contributors.
// Use of this code is governed by the BSD-style license found in LICENSE.txt

#include "mainwindow.h"
#include "geometrycollection.h"

#include <QtCore/QDataStream>
#include <QtCore/QTimer>
#include <QtGui/QApplication>
#include <QtOpenGL/QGLFormat>

#include "argparse.h"
#include "config.h"
#include "displazserver.h"

class Geometry;


static QStringList g_initialFileNames;
static int storeFileName (int argc, const char *argv[])
{
    for(int i = 0; i < argc; ++i)
        g_initialFileNames.push_back (argv[i]);
    return 0;
}


/// Set up search paths to our application directory for Qt's file search
/// mechanism.
///
/// This allows us to use "shaders:las_points.glsl" as a path to a shader
/// in the rest of the code, regardless of the system-specific details of how
/// the install directories are laid out.
static void setupQFileSearchPaths()
{
    QString installBinDir = QCoreApplication::applicationDirPath();
    if (!installBinDir.endsWith("/bin"))
    {
        std::cerr << "WARNING: strange install location detected "
                     "- shaders will not be found\n";
        return;
    }
    QString installBaseDir = installBinDir;
    installBaseDir.chop(4);
    QDir::addSearchPath("shaders", installBaseDir + "/" + DISPLAZ_SHADER_DIR);
    QDir::addSearchPath("doc", installBaseDir + "/" + DISPLAZ_DOC_DIR);
}


int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    ArgParse::ArgParse ap;
    bool printVersion = false;
    bool printHelp = false;
    int maxPointCount = 200000000;
    std::string serverName;
    double yaw = -DBL_MAX, pitch = -DBL_MAX, roll = -DBL_MAX;
    double viewRadius = -DBL_MAX;

    std::string shaderName;
    bool useServer = true;

    bool clearFiles = false;
    bool addFiles = false;
    bool rmTemp = false;
    bool quitRemote = false;
    bool queryCursor = false;

    ap.options(
        "displaz - A lidar point cloud viewer\n"
        "Usage: displaz [opts] [file1.las ...]",
        "%*", storeFileName, "",

        "<SEPARATOR>", "\nInitial settings / remote commands:",
        "-maxpoints %d", &maxPointCount, "Maximum number of points to load at a time",
        "-noserver %!",  &useServer,     "Don't attempt to open files in existing window",
        "-server %s",    &serverName,    "Name of displaz instance to message on startup",
        "-shader %s",    &shaderName,    "Name of shader file to load on startup",
        "-viewangles %F %F %F", &yaw, &pitch, &roll, "Set view angles in degrees [yaw, pitch, roll]",
        "-viewradius %F", &viewRadius,   "Set distance to view point",
        "-clear",        &clearFiles,    "Remote: clear all currently loaded files",
        "-quit",         &quitRemote,    "Remote: close the existing displaz window",
        "-add",          &addFiles,      "Remote: add files to currently open set",
        "-rmtemp",       &rmTemp,        "*Delete* files after loading - use with caution to clean up single-use temporary files after loading",
        "-querycursor",  &queryCursor,   "Query 3D cursor location from displaz instance",

        "<SEPARATOR>", "\nAdditional information:",
        "-version",      &printVersion,  "Print version number",
        "-help",         &printHelp,     "Print command line usage help",
        NULL
    );

    attachToParentConsole();
    if(ap.parse(argc, const_cast<const char**>(argv)) < 0)
    {
        ap.usage();
        std::cerr << "ERROR: " << ap.geterror() << std::endl;
        return EXIT_FAILURE;
    }

    if (printVersion)
    {
        std::cout << "version " DISPLAZ_VERSION_STRING "\n";
        return EXIT_SUCCESS;
    }
    if (printHelp)
    {
        ap.usage();
        return EXIT_SUCCESS;
    }

    setupQFileSearchPaths();

    qRegisterMetaType<std::shared_ptr<Geometry>>("std::shared_ptr<Geometry>");

    // TODO: Factor out this socket comms code - sending and recieving of
    // messages should happen in a centralised place.
    QString socketName = DisplazServer::socketName(QString::fromStdString(serverName));
    if (useServer)
    {
        QDir currentDir = QDir::current();
        QLocalSocket socket;
        // Attempt to locate a running displaz instance
        socket.connectToServer(socketName);
        if (socket.waitForConnected(100))
        {
            QByteArray command;
            if (!g_initialFileNames.empty())
            {
                command = addFiles ? "ADD_FILES" : "OPEN_FILES";
                if (rmTemp)
                    command += "\nRMTEMP";
                for (int i = 0; i < g_initialFileNames.size(); ++i)
                {
                    command += "\n";
                    command += currentDir.absoluteFilePath(g_initialFileNames[i]).toUtf8();
                }
            }
            else if (clearFiles)
            {
                command = "CLEAR_FILES";
            }
            else if (yaw != -DBL_MAX)
            {
                command = "SET_VIEW_ANGLES\n" +
                          QByteArray().setNum(yaw)   + "\n" +
                          QByteArray().setNum(pitch) + "\n" +
                          QByteArray().setNum(roll);
            }
            else if (viewRadius != -DBL_MAX)
            {
                command = "SET_VIEW_RADIUS\n" +
                          QByteArray().setNum(viewRadius);
            }
            else if (quitRemote)
            {
                command = "QUIT";
            }
            else if (queryCursor)
            {
                command = "QUERY_CURSOR";
            }
            else
            {
                std::cerr << "WARNING: Existing window found, but no remote "
                             "command specified - exiting\n";
                // Since we opened the connection, close it nicely by sending a
                // zero-length message to say goodbye.
                command = "";
            }
            QDataStream stream(&socket);
            // Writes length as big endian uint32 followed by raw bytes
            stream.writeBytes(command.data(), command.length());
            socket.waitForBytesWritten(10000);
            if (queryCursor)
            {
                QByteArray msg = DisplazServer::readMessage(socket);
                std::cout.write(msg.data(), msg.length());
                std::cout << "\n";
            }
            socket.disconnectFromServer();
            return EXIT_SUCCESS;
        }
        else
        {
            // Some remote commands fail when no instance is found
            if (queryCursor)
            {
                std::cerr << "ERROR: No remote displaz instance found\n";
                return EXIT_FAILURE;
            }
            // Some remote commands succeed when no instance is found
            if (quitRemote || clearFiles)
            {
                return EXIT_SUCCESS;
            }
        }
    }
    else if (quitRemote)
    {
        // If no remote found, assume -quit was successful
        std::cerr << "ERROR: -quit cannot be combined with -noserver\n";
        return EXIT_FAILURE;
    }
    // If we didn't find any other running instance, start up a server to
    // accept incoming connections, if desired
    std::unique_ptr<DisplazServer> server;
    if (useServer)
        server.reset(new DisplazServer(socketName));

    // Multisampled antialiasing - this makes rendered point clouds look much
    // nicer, but also makes the render much slower, especially on lower
    // powered graphics cards.
    //QGLFormat f = QGLFormat::defaultFormat();
    //f.setSampleBuffers(true);
    //QGLFormat::setDefaultFormat(f);

    PointViewerMainWindow window;
    if (useServer)
    {
        QObject::connect(server.get(), SIGNAL(messageReceived(QByteArray, QLocalSocket*)),
                         &window, SLOT(runCommand(QByteArray, QLocalSocket*)));
    }
    window.geometries().setMaxPointCount(maxPointCount);
    if (!shaderName.empty())
        window.openShaderFile(QString::fromStdString(shaderName));
    window.show();
    window.geometries().loadFiles(g_initialFileNames, rmTemp);

    return app.exec();
}

