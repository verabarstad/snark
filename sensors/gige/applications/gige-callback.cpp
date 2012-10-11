#include <boost/bind.hpp>
#include <boost/thread.hpp>
#include <tbb/concurrent_queue.h>
#include <tbb/pipeline.h>
#include <tbb/task_scheduler_init.h>
#include <comma/application/command_line_options.h>
#include <comma/application/signal_flag.h>
#include <comma/base/exception.h>
#include <comma/csv/stream.h>
#include <comma/name_value/map.h>
#include <snark/imaging/cv_mat/filters.h>
#include <snark/imaging/cv_mat/serialization.h>
#include <snark/tbb/queue.h>
#include <snark/sensors/gige/gige.h>
#include <boost/program_options.hpp>

static comma::signal_flag is_shutdown;
static bool verbose;
static unsigned int emptyFrameCounter = 0;
static unsigned int discard_more_than;
typedef std::pair< boost::posix_time::ptime, cv::Mat > Pair;
static snark::tbb::queue< Pair > queue;
static boost::scoped_ptr< snark::camera::gige::callback > callback;
static bool running = true;

static void spin_()
{
    static unsigned int i = 0;
    static char spinner[] = { '-', '/', '|', '\\' };
    if( i % 3 == 0 ) { std::cerr << '\r' << spinner[ i / 3 ] << "   "; }
    if( ++i >= 12 ) { i = 0; }
}

static void on_frame_( const Pair& p ) // quick and dirty
{
    if( p.second.size().width == 0 )
    {
        emptyFrameCounter++;
        if( emptyFrameCounter > 20 )
        {
            COMMA_THROW( comma::exception, "got lots of empty frames, check that the packet size in the camera matches the mtu on your machine" );
        }
        if( verbose )
        {
            std::cerr << "gige-cat: got empty frame" << std::endl;
        }
        return;
    }
    emptyFrameCounter = 0;
    Pair q;
    if( is_shutdown || !running ) { return queue.push( q ); } // to force read exit
    q.first = p.first;
    p.second.copyTo( q.second ); // quick and dirty: full copy; todo: implement circular queue in gige::callback?
    queue.push( q );
    
    if( verbose ) { spin_(); }
    if( discard_more_than > 0 )
    {
        int size = queue.size();
        if( size > 1 )
        {
            int size_to_discard = size - discard_more_than;
            Pair p;
            for( int i = 0; i < size_to_discard; ++i ) { queue.pop( p ); } // clear() is not thread-safe
            if( verbose && size_to_discard > 0 ) { std::cerr << "gige-cat: discarded " << size_to_discard << " frames" << std::endl; }
        }
    }    
}


static Pair read_( tbb::flow_control& flow )
{
    if( queue.empty() || is_shutdown || !callback->good() || !std::cout.good() || !running )
    {
        flow.stop();
        return Pair();
    }
    Pair p;
    queue.pop( p );
    return p;
}

static void write_( snark::cv_mat::serialization& serialization, Pair p )
{
    if( p.second.size().width == 0 )
    {
        running = false;
    }
    static std::vector< char > buffer;
    buffer.resize( serialization.size( p ) );
    serialization.write( std::cout, p );
}

int main( int argc, char** argv )
{
    try
    {
        unsigned int id;
        std::string fields;
        std::string setattributes;
        unsigned int discard;
        boost::program_options::options_description description( "options" );
        description.add_options()
            ( "help,h", "display help message" )
            ( "set", boost::program_options::value< std::string >( &setattributes ), "set camera attributes as comma-separated name-value pairs and exit" )
            ( "id", boost::program_options::value< unsigned int >( &id )->default_value( 0 ), "camera id; default: first available camera" )
            ( "discard,d", "discard frames, if cannot keep up; same as --buffer=1" )
            ( "buffer", boost::program_options::value< unsigned int >( &discard )->default_value( 0 ), "maximum buffer size before discarding frames" )
            ( "fields,f", boost::program_options::value< std::string >( &fields )->default_value( "t,rows,cols,type" ), "header fields, possible values: t,rows,cols,type,size" )
            ( "list-attributes", "output current camera attributes" )
            ( "list-cameras", "list all cameras and exit" )
            ( "verbose,v", "be more verbose" )
            ( "header", "output header only" )
            ( "no-header", "output image data only" );

        boost::program_options::variables_map vm;
        boost::program_options::store( boost::program_options::parse_command_line( argc, argv, description), vm );
        boost::program_options::parsed_options parsed = boost::program_options::command_line_parser(argc, argv).options( description ).allow_unregistered().run();
        boost::program_options::notify( vm );

        if( vm.count( "header" ) + vm.count( "no-header" ) > 1 )
        {
            COMMA_THROW( comma::exception, "--header, and --no-header are mutually exclusive" );
        }
        
        if ( vm.count( "help" ) )
        {
            std::cerr << "acquire images from a prosilica gige camera, same as gige-cat but using a callback " << std::endl;
            std::cerr << "instead of a thread to acquire the images" << std::endl;
            std::cerr << "output to stdout as serialized cv::Mat" << std::endl;
            std::cerr << "usage: gige-capture [<options>] [<filters>]" << std::endl;
            std::cerr << "known bug: freezes on slow consumers even with --discard, use gige-cat instead" << std::endl;
            std::cerr << description << std::endl;
            std::cerr << snark::cv_mat::filters::usage() << std::endl;
            return 1;
        }
        
        verbose = vm.count( "verbose" );
        if( vm.count( "list-cameras" ) )
        {
            const std::vector< tPvCameraInfo >& list = snark::camera::gige::list_cameras();
            for( std::size_t i = 0; i < list.size(); ++i ) // todo: serialize properly with name-value
            {
                std::cout << "id=" << list[i].UniqueId << "," << "name=\"" << list[i].DisplayName << "\"" << "," << "serial=\"" << list[i].SerialString << "\"" << std::endl;
            }
            return 0;
        }
        if ( vm.count( "discard" ) )
        {
            discard = 1;
        }
        discard_more_than = discard;
        snark::camera::gige::attributes_type attributes;
        if( vm.count( "set" ) )
        {
            comma::name_value::map m( setattributes, ',', '=' );
            attributes.insert( m.get().begin(), m.get().end() );
        }
        if( verbose ) { std::cerr << "gige-cat: connecting..." << std::endl; }
        snark::camera::gige gige( id, attributes );
        if( verbose ) { std::cerr << "gige-cat: connected to camera " << gige.id() << std::endl; }
        if( verbose ) { std::cerr << "gige-cat: total bytes per frame: " << gige.total_bytes_per_frame() << std::endl; }
        if( !attributes.empty() ) { return 0; }
        if( vm.count( "list-attributes" ) )
        {
            attributes = gige.attributes(); // quick and dirty
            for( snark::camera::gige::attributes_type::const_iterator it = attributes.begin(); it != attributes.end(); ++it )
            {
                if( it != attributes.begin() ) { std::cout << std::endl; }
                std::cout << it->first;
                if( it->second != "" ) { std::cout << '=' << it->second; }
            }
            return 0;
        }
        std::vector< std::string > v = comma::split( fields, "," );
        comma::csv::format format;
        for( unsigned int i = 0; i < v.size(); ++i )
        {
            if( v[i] == "t" ) { format += "t"; }
            else { format += "ui"; }
        }
        std::vector< std::string > filterStrings = boost::program_options::collect_unrecognized( parsed.options, boost::program_options::include_positional );
        std::string filters;
        if( filterStrings.size() == 1 )
        {
            filters = filterStrings[0];
        }
        if( filterStrings.size() > 1 )
        {
            COMMA_THROW( comma::exception, "please provide filters as name-value string" );
        }
        boost::scoped_ptr< snark::cv_mat::serialization > serialization;
        if( vm.count( "no-header" ) )
        {
            serialization.reset( new snark::cv_mat::serialization( "", format ) );
        }
        else
        {
            serialization.reset( new snark::cv_mat::serialization( fields, format, vm.count( "header" ) ) );
        }       
        callback.reset( new snark::camera::gige::callback( gige, on_frame_ ) );
        tbb::task_scheduler_init init;
        tbb::filter_t< void, Pair > read( tbb::filter::serial_in_order, boost::bind( read_, _1 ) );
        tbb::filter_t< Pair, void > write( tbb::filter::serial_in_order, boost::bind( write_, boost::ref( *serialization), _1 ) );
        tbb::filter_t< void, Pair > imageFilters = read;

        if( !filters.empty() )
        {
            std::vector< snark::cv_mat::filter > cvMatFilters = snark::cv_mat::filters::make( filters );
            for( std::size_t i = 0; i < cvMatFilters.size(); ++i )
            {
                tbb::filter::mode mode = tbb::filter::serial_in_order;
                if( cvMatFilters[i].parallel )
                {
                    mode = tbb::filter::parallel;
                }
                tbb::filter_t< Pair, Pair > filter( mode, boost::bind( cvMatFilters[i].filter_function, _1 ) );
                imageFilters = imageFilters & filter;
            }
        }

        while( !is_shutdown && running )
        {
            tbb::parallel_pipeline( init.default_num_threads(), imageFilters & write );
            queue.wait();
        }
        
        if( is_shutdown && verbose ) { std::cerr << "gige-cat: caught signal" << std::endl; }
        return 0;
    }
    catch( std::exception& ex )
    {
        std::cerr << "gige-cat: " << ex.what() << std::endl;
    }
    catch( ... )
    {
        std::cerr << "gige-cat: unknown exception" << std::endl;
    }
    return 1;
}

