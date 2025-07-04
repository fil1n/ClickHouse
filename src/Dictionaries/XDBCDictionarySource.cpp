#include <Dictionaries/XDBCDictionarySource.h>

#include <Columns/ColumnString.h>
#include <Common/DateLUTImpl.h>
#include <DataTypes/DataTypeString.h>
#include <IO/ReadWriteBufferFromHTTP.h>
#include <IO/WriteHelpers.h>
#include <IO/ConnectionTimeouts.h>
#include <Interpreters/Context.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Util/AbstractConfiguration.h>
#include <Common/LocalDateTime.h>
#include <Common/logger_useful.h>
#include <Core/Settings.h>
#include <Dictionaries/DictionarySourceFactory.h>
#include <Dictionaries/DictionaryStructure.h>
#include <Dictionaries/readInvalidateQuery.h>
#include <Common/escapeForFileName.h>
#include <Core/ServerSettings.h>
#include <QueryPipeline/QueryPipeline.h>
#include <Processors/Formats/IInputFormat.h>
#include "config.h"


namespace DB
{
namespace Setting
{
    extern const SettingsSeconds http_receive_timeout;
    extern const SettingsBool odbc_bridge_use_connection_pooling;

    /// Cloud only
    extern const SettingsBool cloud_mode;
}

namespace ErrorCodes
{
    extern const int SUPPORT_IS_DISABLED;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
}

namespace
{
    ExternalQueryBuilder makeExternalQueryBuilder(const DictionaryStructure & dict_struct_,
                                                  const std::string & db_,
                                                  const std::string & schema_,
                                                  const std::string & table_,
                                                  const std::string & query_,
                                                  const std::string & where_,
                                                  IXDBCBridgeHelper & bridge_)
    {
        QualifiedTableName qualified_name{schema_, table_};

        if (bridge_.isSchemaAllowed())
        {
            if (qualified_name.database.empty())
                qualified_name = QualifiedTableName::parseFromString(qualified_name.table);
        }
        else
        {
            if (!qualified_name.database.empty())
                throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                    "Dictionary source specifies a schema but schema is not supported by {}-driver",
                    bridge_.getName());
        }

        return {dict_struct_, db_, qualified_name.database, qualified_name.table, query_, where_, bridge_.getIdentifierQuotingStyle()};
    }
}

static const UInt64 max_block_size = 8192;


XDBCDictionarySource::XDBCDictionarySource(
    const DictionaryStructure & dict_struct_,
    const Configuration & configuration_,
    const Block & sample_block_,
    ContextPtr context_,
    const BridgeHelperPtr bridge_)
    : WithContext(context_->getGlobalContext())
    , log(getLogger(bridge_->getName() + "DictionarySource"))
    , update_time(std::chrono::system_clock::from_time_t(0))
    , dict_struct(dict_struct_)
    , configuration(configuration_)
    , sample_block(sample_block_)
    , query_builder(makeExternalQueryBuilder(dict_struct, configuration.db, configuration.schema, configuration.table, configuration.query, configuration.where, *bridge_))
    , load_all_query(query_builder.composeLoadAllQuery())
    , bridge_helper(bridge_)
    , bridge_url(bridge_helper->getMainURI())
    , timeouts(ConnectionTimeouts::getHTTPTimeouts(context_->getSettingsRef(), context_->getServerSettings()))
{
    auto url_params = bridge_helper->getURLParams(max_block_size);
    for (const auto & [name, value] : url_params)
        bridge_url.addQueryParameter(name, value);
}

/// copy-constructor is provided in order to support cloneability
XDBCDictionarySource::XDBCDictionarySource(const XDBCDictionarySource & other)
    : WithContext(other.getContext())
    , log(getLogger(other.bridge_helper->getName() + "DictionarySource"))
    , update_time(other.update_time)
    , dict_struct(other.dict_struct)
    , configuration(other.configuration)
    , sample_block(other.sample_block)
    , query_builder(other.query_builder)
    , load_all_query(other.load_all_query)
    , invalidate_query_response(other.invalidate_query_response)
    , bridge_helper(other.bridge_helper)
    , bridge_url(other.bridge_url)
    , timeouts(other.timeouts)
{
}


std::string XDBCDictionarySource::getUpdateFieldAndDate()
{
    if (update_time != std::chrono::system_clock::from_time_t(0))
    {
        time_t hr_time = std::chrono::system_clock::to_time_t(update_time) - configuration.update_lag;
        std::string str_time = DateLUT::instance().timeToString(hr_time);
        update_time = std::chrono::system_clock::now();
        return query_builder.composeUpdateQuery(configuration.update_field, str_time);
    }

    update_time = std::chrono::system_clock::now();
    return load_all_query;
}


QueryPipeline XDBCDictionarySource::loadAll()
{
    LOG_TRACE(log, fmt::runtime(load_all_query));
    return loadFromQuery(bridge_url, sample_block, load_all_query);
}


QueryPipeline XDBCDictionarySource::loadUpdatedAll()
{
    std::string load_query_update = getUpdateFieldAndDate();

    LOG_TRACE(log, fmt::runtime(load_query_update));
    return loadFromQuery(bridge_url, sample_block, load_query_update);
}


QueryPipeline XDBCDictionarySource::loadIds(const std::vector<UInt64> & ids)
{
    const auto query = query_builder.composeLoadIdsQuery(ids);
    return loadFromQuery(bridge_url, sample_block, query);
}


QueryPipeline XDBCDictionarySource::loadKeys(const Columns & key_columns, const std::vector<size_t> & requested_rows)
{
    const auto query = query_builder.composeLoadKeysQuery(key_columns, requested_rows, ExternalQueryBuilder::AND_OR_CHAIN);
    return loadFromQuery(bridge_url, sample_block, query);
}


bool XDBCDictionarySource::supportsSelectiveLoad() const
{
    return true;
}


bool XDBCDictionarySource::hasUpdateField() const
{
    return !configuration.update_field.empty();
}


DictionarySourcePtr XDBCDictionarySource::clone() const
{
    return std::make_shared<XDBCDictionarySource>(*this);
}


std::string XDBCDictionarySource::toString() const
{
    const auto & where = configuration.where;
    return bridge_helper->getName() + ": " + configuration.db + '.' + configuration.table + (where.empty() ? "" : ", where: " + where);
}


bool XDBCDictionarySource::isModified() const
{
    if (!configuration.invalidate_query.empty())
    {
        auto response = doInvalidateQuery(configuration.invalidate_query);
        if (invalidate_query_response == response)
            return false;
        invalidate_query_response = response;
    }
    return true;
}


std::string XDBCDictionarySource::doInvalidateQuery(const std::string & request) const
{
    Block invalidate_sample_block;
    ColumnPtr column(ColumnString::create());
    invalidate_sample_block.insert(ColumnWithTypeAndName(column, std::make_shared<DataTypeString>(), "Sample Block"));

    bridge_helper->startBridgeSync();

    auto invalidate_url = bridge_helper->getMainURI();
    auto url_params = bridge_helper->getURLParams(max_block_size);
    for (const auto & [name, value] : url_params)
        invalidate_url.addQueryParameter(name, value);

    return readInvalidateQuery(QueryPipeline(loadFromQuery(invalidate_url, invalidate_sample_block, request)));
}


QueryPipeline XDBCDictionarySource::loadFromQuery(const Poco::URI & uri, const Block & required_sample_block, const std::string & query) const
{
    bridge_helper->startBridgeSync();

    auto write_body_callback = [required_sample_block, query](std::ostream & os)
    {
        os << "sample_block=" << escapeForFileName(required_sample_block.getNamesAndTypesList().toString());
        os << "&";
        os << "query=" << escapeForFileName(query);
    };

    auto buf = BuilderRWBufferFromHTTP(uri)
                   .withConnectionGroup(HTTPConnectionGroupType::STORAGE)
                   .withMethod(Poco::Net::HTTPRequest::HTTP_POST)
                   .withTimeouts(timeouts)
                   .withOutCallback(std::move(write_body_callback))
                   .create(credentials);

    auto format = getContext()->getInputFormat(IXDBCBridgeHelper::DEFAULT_FORMAT, *buf, required_sample_block, max_block_size);
    format->addBuffer(std::move(buf));

    return QueryPipeline(std::move(format));
}

void registerDictionarySourceXDBC(DictionarySourceFactory & factory)
{
    auto create_table_source = [=](const String & /*name*/,
                                   const DictionaryStructure & dict_struct,
                                   const Poco::Util::AbstractConfiguration & config,
                                   const std::string & config_prefix,
                                   Block & sample_block,
                                   ContextPtr global_context,
                                   const std::string & /* default_database */,
                                   bool /* check_config */) -> DictionarySourcePtr {

        if (global_context->getSettingsRef()[Setting::cloud_mode])
            throw Exception(ErrorCodes::SUPPORT_IS_DISABLED, "Dictionary source of type `odbc` is disabled");

        BridgeHelperPtr bridge = std::make_shared<XDBCBridgeHelper<ODBCBridgeMixin>>(
            global_context,
            global_context->getSettingsRef()[Setting::http_receive_timeout],
            config.getString(config_prefix + ".odbc.connection_string"),
            config.getBool(config_prefix + ".settings.odbc_bridge_use_connection_pooling",
            global_context->getSettingsRef()[Setting::odbc_bridge_use_connection_pooling]));

        std::string settings_config_prefix = config_prefix + ".odbc";

        XDBCDictionarySource::Configuration configuration
        {
            .db = config.getString(settings_config_prefix + ".db", ""),
            .schema = config.getString(settings_config_prefix + ".schema", ""),
            .table = config.getString(settings_config_prefix + ".table", ""),
            .query = config.getString(settings_config_prefix + ".query", ""),
            .where = config.getString(settings_config_prefix + ".where", ""),
            .invalidate_query = config.getString(settings_config_prefix + ".invalidate_query", ""),
            .update_field = config.getString(settings_config_prefix + ".update_field", ""),
            .update_lag = config.getUInt64(settings_config_prefix + ".update_lag", 1)
        };

        return std::make_unique<XDBCDictionarySource>(dict_struct, configuration, sample_block, global_context, bridge);
    };
    factory.registerSource("odbc", create_table_source);
}


void registerDictionarySourceJDBC(DictionarySourceFactory & factory)
{
    auto create_table_source = [=](const String & /*name*/,
                                 const DictionaryStructure & /* dict_struct */,
                                 const Poco::Util::AbstractConfiguration & /* config */,
                                 const std::string & /* config_prefix */,
                                 Block & /* sample_block */,
                                 ContextPtr /* global_context */,
                                 const std::string & /* default_database */,
                                 bool /* created_from_ddl */) -> DictionarySourcePtr {
        throw Exception(ErrorCodes::SUPPORT_IS_DISABLED,
            "Dictionary source of type `jdbc` is disabled until consistent support for nullable fields.");
    };
    factory.registerSource("jdbc", create_table_source);
}

}
