// **********************************************************************
//
// Copyright (c) 2002
// Mutable Realms, Inc.
// Huntsville, AL, USA
//
// All Rights Reserved
//
// **********************************************************************

#include <IceUtil/IceUtil.h>
#include <Ice/Application.h>
#include <IcePatch/FileLocator.h>
#include <IcePatch/Util.h>
#ifdef _WIN32
#   include <direct.h>
#endif

using namespace std;
using namespace Ice;
using namespace IcePatch;

namespace IcePatch
{

class Server : public Application
{
public:

    void usage();
    virtual int run(int, char*[]);
};

class Updater : public IceUtil::Thread, public IceUtil::Monitor<IceUtil::Mutex>
{
public:

    Updater(const ObjectAdapterPtr&, const IceUtil::Time&);

    virtual void run();
    void destroy();

protected:

    const ObjectAdapterPtr _adapter;
    const LoggerPtr _logger;
    const IceUtil::Time _updatePeriod;
    bool _destroy;

    void cleanup(const FileDescSeq&);
};

typedef IceUtil::Handle<Updater> UpdaterPtr;

};

void
IcePatch::Server::usage()
{
    cerr << "Usage: " << appName() << " [options]\n";
    cerr <<     
        "Options:\n"
        "-h, --help           Show this message.\n"
        "-v, --version        Display the Ice version.\n"
        ;
}

int
IcePatch::Server::run(int argc, char* argv[])
{
    for(int i = 1; i < argc; ++i)
    {
	if(strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
	{
	    usage();
	    return EXIT_SUCCESS;
	}
	else if(strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0)
	{
	    cout << ICE_STRING_VERSION << endl;
	    return EXIT_SUCCESS;
	}
	else
	{
	    cerr << appName() << ": unknown option `" << argv[i] << "'" << endl;
	    usage();
	    return EXIT_FAILURE;
	}
    }
    
    PropertiesPtr properties = communicator()->getProperties();
    
    //
    // Get the IcePatch endpoints.
    //
    const char* endpointsProperty = "IcePatch.Endpoints";
    string endpoints = properties->getProperty(endpointsProperty);
    if(endpoints.empty())
    {
	cerr << appName() << ": property `" << endpointsProperty << "' is not set" << endl;
	return EXIT_FAILURE;
    }
    
    //
    // Get the working directory and change to this directory.
    //
    const char* directoryProperty = "IcePatch.Directory";
    string directory = properties->getProperty(directoryProperty);
    if(!directory.empty())
    {
#ifdef _WIN32
	if(_chdir(directory.c_str()) == -1)
#else
	if(chdir(directory.c_str()) == -1)
#endif
	{
	    cerr << appName() << ": cannot change to directory `" << directory << "': " << strerror(errno) << endl;
            return EXIT_FAILURE;
	}
    }
    
    //
    // Create and initialize the object adapter and the file locator.
    //
    ObjectAdapterPtr adapter = communicator()->createObjectAdapterFromProperty("IcePatch", endpointsProperty);
    ServantLocatorPtr fileLocator = new FileLocator(adapter);
    adapter->addServantLocator(fileLocator, "IcePatch");
    
    //
    // Start the updater if an update period is set.
    //
    UpdaterPtr updater;
    IceUtil::Time updatePeriod = IceUtil::Time::seconds(
	properties->getPropertyAsIntWithDefault("IcePatch.UpdatePeriod", 60));
    if(updatePeriod != IceUtil::Time())
    {
	if(updatePeriod < IceUtil::Time::seconds(10))
	{
	    updatePeriod = IceUtil::Time::seconds(10);
	}
	updater = new Updater(adapter, updatePeriod);
	updater->start();
    }

    //
    // Everything ok, let's go.
    //
    shutdownOnInterrupt();
    adapter->activate();
    communicator()->waitForShutdown();
    ignoreInterrupt();

    //
    // Destroy and join with the updater, if there is one.
    //
    if(updater)
    {
	updater->destroy();
	updater->getThreadControl().join();
    }

    return EXIT_SUCCESS;
}

IcePatch::Updater::Updater(const ObjectAdapterPtr& adapter, const IceUtil::Time& updatePeriod) :
    _adapter(adapter),
    _logger(_adapter->getCommunicator()->getLogger()),
    _updatePeriod(updatePeriod),
    _destroy(false)
{
}

void
IcePatch::Updater::run()
{
    IceUtil::Monitor<IceUtil::Mutex>::Lock sync(*this);

    while(!_destroy)
    {
	try
	{
	    Identity identity = pathToIdentity(".");
	    ObjectPrx topObj = _adapter->createProxy(identity);
	    FilePrx top = FilePrx::checkedCast(topObj);
	    assert(top);
	    DirectoryDescPtr topDesc = DirectoryDescPtr::dynamicCast(top->describe());
	    assert(topDesc);
	    cleanup(topDesc->directory->getContents());
	}
	catch(const FileAccessException& ex)
	{
	    Error out(_logger);
	    out << "exception during update:\n" << ex << ":\n" << ex.reason;
	}
	catch(const BusyException&)
	{
	    //
	    // Just loop if we're busy.
	    //
	}
	catch(const Exception& ex)
	{
	    //
	    // If we are interrupted due to a shutdown, don't print
	    // any exceptions. Exceptions are normal in such case, for
	    // example, ObjectAdapterDeactivatedException.
	    //
	    if(!Application::isShutdownFromInterrupt())
	    {
		Error out(_logger);
		out << "exception during update:\n" << ex;
	    }
	}

	if(_destroy)
	{
	    break;
	}

	timedWait(_updatePeriod);
    }
}

void
IcePatch::Updater::destroy()
{
    IceUtil::Monitor<IceUtil::Mutex>::Lock sync(*this);
    _destroy = true;
    notify();
}

void
IcePatch::Updater::cleanup(const FileDescSeq& fileDescSeq)
{
    if(_destroy)
    {
	return;
    }

    for(FileDescSeq::const_iterator p = fileDescSeq.begin(); p != fileDescSeq.end(); ++p)
    {
	DirectoryDescPtr directoryDesc = DirectoryDescPtr::dynamicCast(*p);
	if(directoryDesc)
	{
	    //
	    // Force .md5 files to be created and orphaned files to be
	    // removed.
	    //
	    cleanup(directoryDesc->directory->getContents());
	}
	else
	{
	    RegularDescPtr regularDesc = RegularDescPtr::dynamicCast(*p);
	    assert(regularDesc);

	    //
	    // Force .bz2 files to be created.
	    //
	    regularDesc->regular->getBZ2Size();
	}
    }
}

int
main(int argc, char* argv[])
{
    PropertiesPtr defaultProperties;
    try
    {
	defaultProperties = getDefaultProperties(argc, argv);
        StringSeq args = argsToStringSeq(argc, argv);
        args = defaultProperties->parseCommandLineOptions("IcePatch", args);
        stringSeqToArgs(args, argc, argv);
    }
    catch(const Exception& ex)
    {
	cerr << argv[0] << ": " << ex << endl;
	return EXIT_FAILURE;
    }

    Server app;
    return app.main(argc, argv);
}
