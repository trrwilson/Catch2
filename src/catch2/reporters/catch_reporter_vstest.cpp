/*
 *  Distributed under the Boost Software License, Version 1.0. (See accompanying
 *  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 */

#include <catch2/reporters/catch_reporter_vstest.hpp>
#include <catch2/reporters/catch_reporter_helpers.hpp>
#include <catch2/interfaces/catch_interfaces_config.hpp>
#include <catch2/catch_test_spec.hpp>
#include <catch2/catch_tostring.hpp>
#include <catch2/internal/catch_enforce.hpp>
#include <catch2/internal/catch_list.hpp>
#include <catch2/internal/catch_string_manip.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>

namespace Catch {

    namespace { // anonymous namespace/this file only

        // Several elements in Vstest require globally unique IDs (GUIDs). Here we use a random generation algorithm
        // that's *not* guaranteed to be truly globally unique, but should be "unique enough" for all reasonable
        // purposes that aren't correlating hundreds of thousands of test runs.
        std::string get_random_not_guaranteed_unique_guid() {
            auto get_random_uint = []() {
                std::random_device random_device;
                std::mt19937 random_generator(random_device());
                std::uniform_int_distribution<unsigned int> random_distribution(
                    std::numeric_limits<unsigned int>::min(),
                    std::numeric_limits<unsigned int>::max());
                return random_distribution(random_generator);
            };

            std::ostringstream guid_stream;

            bool is_first_segment{ true };
            for (const auto segmentLength : { 8, 4, 4, 4, 12 } ) {
                guid_stream << (is_first_segment ? "" : "-");
                is_first_segment = false;

                for (int i = 0; i < segmentLength; i++) {
                    guid_stream << std::hex << ( get_random_uint() % 16 );
                }
            }

            return guid_stream.str();
        }

        std::string nanosToDurationString( unsigned long long nanos ) {
            auto totalHns = nanos / 100;
            auto totalSeconds = nanos / 1000000000;
            auto totalMinutes = totalSeconds / 60;
            auto totalHours = totalMinutes / 60;
            ReusableStringStream resultStream;
            resultStream
                << std::setfill('0') << std::setw(2) << totalHours % 60 << ":"
                << std::setfill('0') << std::setw(2) << totalMinutes % 60 << ":"
                << std::setfill('0') << std::setw(2) << totalSeconds % 60 << "."
                << std::setfill('0') << std::setw(-7) << totalHns % 10000000;
            return resultStream.str();
        }

        // Some consumers of output .trx files (e.g. Azure DevOps Pipelines) fail to ingest results
        // from .trx files if they have certain characters in them. This removes those characters.
        // To-do: make this a parameter or address the root problem of consumers being weird
        std::string getSanitizedTrxName( const std::string& rawName ) {
            ReusableStringStream resultStream;
            auto lastChar = '\0';
            for ( size_t i = 0; i < rawName.length(); ) {
                if ( rawName[i] == '[' ) {
                    if ( rawName.find( ']', i ) == std::string::npos ) {
                        CATCH_ERROR( "Unclosed [tag] in name: " << rawName );
                    }
                    do {
                        i++;
                    } while ( rawName[i - 1] != ']' );
                    if ( lastChar == ' ' && rawName[i] == ' ' ) {
                        // "removed [tag] here" -> "removed  tag" -> "removed tag"
                        i++;
                    }
                } else if ( rawName[i] == ',' ) {
                    i++;
                } else {
                    lastChar = rawName[i];
                    resultStream << lastChar;
                    i++;
                }
            }
            return trim( resultStream.str() );
        }

    } // namespace

    VstestResult::VstestResult() 
        : testId{ get_random_not_guaranteed_unique_guid() }
        , testExecutionId{ get_random_not_guaranteed_unique_guid() }
    {}

    std::vector<VstestResult> VstestResult::parseTraversals( const std::vector<SectionTraversalRef>& traversals ) {
        std::vector<VstestResult> results;
        auto isNewTraversalRoot = [&]( const VstestResult& result, const IncrementalSectionTraversal& traversal ) -> bool {
            auto lastTraversalPtr = result.traversals.empty()
                ? nullptr 
                : &( result.traversals[ result.traversals.size() - 1 ] );
            return !lastTraversalPtr 
                || lastTraversalPtr->get().allSectionInfo.empty()
                || traversal.allSectionInfo.empty()
                || lastTraversalPtr->get().allSectionInfo[0].name != traversal.allSectionInfo[0].name;
        };
        for ( size_t i = 0; i < traversals.size(); ) {
            VstestResult newResult{};
            do {
                newResult.traversals.push_back( traversals[i] );
                i++;
            } while ( i < traversals.size() && !isNewTraversalRoot( newResult, traversals[i] ) );
            results.push_back( newResult );
        }
        return results;
    }

    bool VstestResult::isOk() const {
        return std::all_of(
            traversals.begin(),
            traversals.end(),
            []( const IncrementalSectionTraversal& traversal ) {
                return traversal.isOk();
            } );
    }

    std::string VstestResult::getRootTestName() const {
        return traversals.empty() || traversals[0].get().allSectionInfo.empty()
            ? ""
            : traversals[0].get().allSectionInfo[0].name;
    }

    std::string VstestResult::getRootRunName() const {
        return traversals.empty() ? "" : traversals[0].get().testRunInfo.name;
    }

    const std::vector<Catch::Tag> VstestResult::getRootTestTags() const {
        return traversals.empty() ? std::vector<Catch::Tag> {} : traversals[0].get().testTags;
    }

    std::chrono::system_clock::time_point VstestResult::getStartTime() const {
        return traversals.empty() || !traversals[0].get().isComplete()
            ? std::chrono::system_clock::now()
            : traversals[0].get().startTime;
    }

    std::chrono::system_clock::time_point VstestResult::getFinishTime() const {
        return traversals.empty() || !traversals[ traversals.size() - 1 ].get().isComplete()
            ? std::chrono::system_clock::now()
            : traversals[ traversals.size() - 1 ].get().finishTime;
    }

    VstestTrxDocument::VstestTrxDocument(
            std::ostream& stream,
            std::vector<VstestResult>& results,
            const std::string& sourcePathPrefix,
            const std::vector<std::string>& attachmentPaths )
        : m_xml{ stream }
        , m_results{ std::move(results) }
        , m_sourcePrefix{ sourcePathPrefix }
        , m_attachmentPaths{ attachmentPaths }
        , m_defaultTestListId{ get_random_not_guaranteed_unique_guid() }
    {}

    void VstestTrxDocument::serialize(
            std::ostream& stream,
            std::vector<VstestResult>& results,
            const std::string& sourcePathPrefix,
            const std::vector<std::string>& attachmentPaths ) {
        VstestTrxDocument trx{ stream, results, sourcePathPrefix, attachmentPaths };
        trx.startWriteTestRun();
        trx.writeTimes();
        trx.writeResults();
        trx.writeTestDefinitions();
        trx.writeTestLists();
        trx.writeTestEntries();
        trx.writeSummary( attachmentPaths );
        trx.m_xml.endElement(); // TestRun
    }

    void VstestTrxDocument::startWriteTestRun() {
        auto runName = m_results.empty() || m_results[0].traversals.empty()
            ? ""
            : m_results[0].traversals[0].get().testRunInfo.name;
        m_xml.startElement( "TestRun" );
        m_xml.writeAttribute( "id", get_random_not_guaranteed_unique_guid() );
        m_xml.writeAttribute( "name", runName );
        m_xml.writeAttribute( "runUser", "Catch2VstestReporter" );
        m_xml.writeAttribute(
            "xmlns",
            "http://microsoft.com/schemas/VisualStudio/TeamTest/2010" );
    }

    void VstestTrxDocument::writeTimes() {
        auto now = std::chrono::system_clock::now();
        auto startTime = now;
        auto finishTime = now;

        if ( !m_results.empty() && !m_results[0].traversals.empty() ) {
            startTime = m_results[0].getStartTime();
            finishTime = m_results[ m_results.size() - 1 ].getFinishTime();
        }

        m_xml.scopedElement( "Times" )
            .writeAttribute( "creation", Catch::Detail::stringify( startTime ) )
            .writeAttribute( "queuing", Catch::Detail::stringify( startTime ) )
            .writeAttribute( "start", Catch::Detail::stringify( startTime ) )
            .writeAttribute( "finish", Catch::Detail::stringify( finishTime ) );
    }

    void VstestTrxDocument::writeResults() {
        m_xml.startElement( "Results" );
        for ( auto& result : m_results ) {
            if ( !result.traversals.empty() ) {
                writeTopLevelResult( result );
            }
        }
        m_xml.endElement(); // Results
    }

    void VstestTrxDocument::writeTopLevelResult( const VstestResult& result ) {
        startWriteTestResult( result );
        writeTimestampAttributes( result.getStartTime(), result.getFinishTime() );
        m_xml.writeAttribute( "outcome", result.isOk() ? "Passed" : "Failed" );

        if ( result.traversals.size() == 1 ) {
            writeTraversalOutput( result.traversals[0] );
        } else {
            m_xml.writeAttribute( "resultType", "DataDrivenTest" );
            for ( const auto& traversal : result.traversals ) {
                writeInnerResult( result, traversal );
            }
        }

        m_xml.endElement(); // UnitTestResult
    }

    void VstestTrxDocument::writeTimestampAttributes(
            std::chrono::system_clock::time_point start,
            std::chrono::system_clock::time_point finish ) {
        auto elapsed = finish - start;
        auto elapsedNanos = std::chrono::duration_cast<std::chrono::nanoseconds>( elapsed ).count();
        m_xml.writeAttribute( "startTime", Catch::Detail::stringify( start ) );
        m_xml.writeAttribute( "endTime", Catch::Detail::stringify( finish ) );
        m_xml.writeAttribute( "duration", nanosToDurationString( elapsedNanos ) );
    }

    void VstestTrxDocument::startWriteTestResult( const VstestResult& result ) {
        startWriteTestResult(
            result.testId,
            result.testExecutionId,
            result.traversals[0].get().allSectionInfo[0].name );
    }

    void VstestTrxDocument::startWriteTestResult(
            const std::string& testId,
            const std::string& testExecutionId,
            const std::string& testName ) {
        constexpr auto computerName = "localhost";
        constexpr auto vsTestTypeId = "13cdc9d9-ddb5-4fa4-a97d-d965ccfc6d4b";

        m_xml.startElement( "UnitTestResult" );
        m_xml.writeAttribute( "executionId", testExecutionId );
        m_xml.writeAttribute( "testId", testId );
        m_xml.writeAttribute( "testName", testName );
        m_xml.writeAttribute( "computerName", computerName );
        m_xml.writeAttribute( "testType", vsTestTypeId );
        m_xml.writeAttribute( "testListId", m_defaultTestListId );
    }

    void VstestTrxDocument::writeTraversalOutput( const IncrementalSectionTraversal& traversal ) {
        auto writeIfPresentOr = [&]( const char* elementName, const std::string& value, bool doAlways = false ) {
            if ( doAlways || !value.empty() ) {
                m_xml.scopedElement( elementName ).writeText( value, XmlFormatting::Newline );
            }
        };

        auto stdOut = traversal.stdOutStream.str();
        auto stdErr = traversal.stdErrStream.str();

        if ( !traversal.isOk() || !stdOut.empty() || !stdErr.empty() ) {
            auto outputElement = m_xml.scopedElement( "Output" );
            writeIfPresentOr( "StdOut", traversal.stdOutStream.str(), !traversal.isComplete() );
            writeIfPresentOr( "StdErr", traversal.stdErrStream.str(), !traversal.isComplete() );
            auto errorMessage = getErrorMessageForTraversal( traversal );
            auto stackMessage = getStackMessageForTraversal( traversal );
            if ( !errorMessage.empty() || !stackMessage.empty() ) {
                auto errorInfoElement = m_xml.scopedElement( "ErrorInfo" );
                writeIfPresentOr( "Message", errorMessage );
                writeIfPresentOr( "StackTrace", stackMessage );
            }
        }
    }

    void VstestTrxDocument::writeInnerResult(
            const VstestResult& result,
            const IncrementalSectionTraversal& traversal ) {
        startWriteTestResult(
            get_random_not_guaranteed_unique_guid(),
            get_random_not_guaranteed_unique_guid(),
            getFullTestNameForTraversal( traversal ) );
        m_xml.writeAttribute( "parentExecutionId", result.testExecutionId );
        m_xml.writeAttribute( "resultType", "DataDrivenDataRow" );
        writeTimestampAttributes( traversal.startTime, traversal.finishTime );
        m_xml.writeAttribute( "outcome", traversal.isOk() ? "Passed" : "Failed" );
        writeTraversalOutput( traversal );
        m_xml.endElement(); // UnitTestResult
    }

    void VstestTrxDocument::writeTestDefinitions() {
        auto testDefinitionsElement = m_xml.scopedElement( "TestDefinitions" );
        for ( const auto& result : m_results ) {
            auto unitTestElement = m_xml.scopedElement( "UnitTest" );
            m_xml.writeAttribute( "name", result.getRootTestName() );
            m_xml.writeAttribute( "storage", result.getRootRunName() );
            m_xml.writeAttribute( "id", result.testId );

            auto testTags = result.traversals[0].get().testTags;
            if ( !result.traversals[0].get().testTags.empty() ) {
                auto testCategoriesElement = m_xml.scopedElement( "TestCategory" );
                for ( const auto& tag : result.traversals[0].get().testTags ) {
                    m_xml.scopedElement( "TestCategoryItem" )
                        .writeAttribute( "TestCategory", tag.original );
                }
            }
            m_xml.scopedElement( "Execution" )
                .writeAttribute( "id", result.testExecutionId );
            m_xml.scopedElement( "TestMethod" )
                .writeAttribute( "codeBase", result.getRootRunName() )
                .writeAttribute( "adapterTypeName", "executor://mstestadapter/v2" )
                .writeAttribute( "className", "Catch2.Test" )
                .writeAttribute( "name", result.getRootTestName() );
        }
    }

    void VstestTrxDocument::writeTestEntries() {
        auto testEntriesElement = m_xml.scopedElement( "TestEntries" );
        for ( const auto& result : m_results ) {
            m_xml.scopedElement( "TestEntry" )
                .writeAttribute( "testId", result.testId )
                .writeAttribute( "executionId", result.testExecutionId )
                .writeAttribute( "testListId", m_defaultTestListId );
        }
    }

    void VstestTrxDocument::writeTestLists() {
        auto testListsElement = m_xml.scopedElement( "TestLists" );
        m_xml.scopedElement( "TestList" )
            .writeAttribute( "name", "Default test list for Catch2" )
            .writeAttribute( "id", m_defaultTestListId );
    }

    void VstestTrxDocument::writeSummary( const std::vector<std::string>& attachmentPaths ) {
        auto resultSummaryElement = m_xml.scopedElement( "ResultSummary" );

        auto runHasFailures = std::any_of(
            m_results.begin(),
            m_results.end(),
            []( const VstestResult& result ) { return !result.isOk(); });
        resultSummaryElement.writeAttribute( "outcome", runHasFailures ? "Failed" : "Passed" );

        if ( !attachmentPaths.empty() ) {
            auto resultFilesElement = m_xml.scopedElement( "ResultFiles" );
            for ( const auto& attachmentPath : attachmentPaths ) {
                m_xml.scopedElement( "ResultFile" ).writeAttribute( "path", attachmentPath );
            }
        }
    }

    std::string VstestTrxDocument::getErrorMessageForTraversal( const IncrementalSectionTraversal& traversal ) {
        std::ostringstream errorStream;
        if ( !traversal.isComplete() ) {
            errorStream 
                << "Test execution terminated unexpectedly before this test completed. Please see redirected output,"
                << " if available, for more details." << "\n";
        }
        for ( const auto& assertionWithExpansion : traversal.allAssertionsWithExpansions ) {
            const auto& result = assertionWithExpansion.first.assertionResult;
            if ( result.getResultType() == ResultWas::ExpressionFailed ) {
                // Here we'll write the failure and also its expanded form, e.g.:
                //  REQUIRE( x == 1 ) as REQUIRE( 2 == 1 )
                errorStream << result.getExpressionInMacro();

                if ( result.getExpression() != assertionWithExpansion.second ) {
                    errorStream << " as "
                        << result.getTestMacroName() << " ( " << assertionWithExpansion.second << " ) " << '\n';
                }
            } else if ( result.getResultType() == ResultWas::ThrewException ) {
                errorStream << "Exception: " << result.getMessage() << '\n';
            } else if ( !result.isOk() ) {
                errorStream << "Failed: " << result.getMessage() << '\n';
            }
        }
        if ( !traversal.fatalSignalName.empty() ) {
            auto& source = traversal.fatalSignalSourceInfo;
            errorStream << "Fatal error: " << traversal.fatalSignalName << " at ";
            serializeSourceInfo( errorStream, source.first, source.second );
        }

        return errorStream.str();
    }

    std::string VstestTrxDocument::getStackMessageForTraversal( const IncrementalSectionTraversal& traversal ) {
        std::ostringstream stackStream;
        for ( const auto& assertionWithExpansion : traversal.allAssertionsWithExpansions ) {
            const auto info = assertionWithExpansion.first.assertionResult.getSourceInfo();
            serializeSourceInfo( stackStream, info.file, info.line );
        }
        if ( !traversal.isComplete() && !traversal.allSectionInfo.empty() ) {
            const auto& lastSection = traversal.allSectionInfo[ traversal.allSectionInfo.size() - 1 ];
            serializeSourceInfo( stackStream, lastSection.lineInfo.file, lastSection.lineInfo.line );
        }
        return stackStream.str();
    }

    std::string VstestTrxDocument::getFullTestNameForTraversal( const IncrementalSectionTraversal& traversal ) {
        std::ostringstream nameStream;
        for ( size_t i = 0; i < traversal.allSectionInfo.size(); i++ ) {
            nameStream << ( i > 0 ? " / " : "" )
                << getSanitizedTrxName( traversal.allSectionInfo[i].name );
        }
        return nameStream.str();
    }

    void VstestTrxDocument::serializeSourceInfo(
            std::ostringstream& stream,
            const std::string& file,
            const size_t line ) {
        auto start = m_sourcePrefix.compare( 0, std::string::npos, file, m_sourcePrefix.length() ) == 0
            ? m_sourcePrefix.length()
            : 0;
        stream << "at Catch.Module.Method() in ";
        for ( auto& c : file.substr( start ) ) {
            stream << ( c == '\\' ? '/' : c );
        }
        stream << ":line " << line << '\n';
    }

    VstestReporter::VstestReporter( ReporterConfig const& _config ) : IncrementalReporterBase{ _config } {
        m_preferences.shouldRedirectStdOut = true;
        m_preferences.shouldReportAllAssertions = true;
        m_config = _config.fullConfig();
    }

    std::string VstestReporter::getDescription() {
        return "Reports test results in .trx XML format, conformant to Vstest v2";
    }

    void VstestReporter::sectionStarting( const SectionInfo& sectionInfo ) {
        IncrementalReporterBase::sectionStarting( sectionInfo );
        if ( incrementalOutputSupported() ) {
            resetIncrementalOutput();
            emitNewTrx( getTraversals() );
        }
    }

    void VstestReporter::sectionTraversalEnded( std::vector<SectionTraversalRef> traversals ) {
        if ( incrementalOutputSupported() ) {
            resetIncrementalOutput();
            emitNewTrx( traversals );
        }
    }

    void VstestReporter::testRunEnded( const Catch::TestRunStats& ) {
        if ( !incrementalOutputSupported() ) {
            emitNewTrx( getTraversals() );
        }
    }

    void VstestReporter::emitNewTrx( const std::vector<SectionTraversalRef>& traversals ) {
        auto results = VstestResult::parseTraversals( traversals );
        VstestTrxDocument::serialize( m_outputStreamRef.get(),
                                        results,
                                        m_config->sourcePathPrefix(),
                                        m_config->reportAttachmentPaths() );
    }

} // end namespace Catch