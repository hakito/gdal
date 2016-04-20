#include "Dataset.h"

#include <fstream>
#include <mutex>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem.hpp>
#include <boost/property_tree/json_parser.hpp>

#include "ogr_spatialref.h"

#include "RasterBand.h"
#include "WarningsReporter.h"

using namespace boost::property_tree;
namespace bfs = boost::filesystem;

namespace aircom { namespace pred_raster {

namespace {

wptree loadJson(const bfs::path& path)
{
	std::wifstream jsonStream(path.wstring());
	wptree tree;
	read_json(jsonStream, tree);
	return tree;
}

Auxiliary parseOrLoadAuxiliary(const wptree& gapTree, ApiWrapper& wrapper, Warnings& warnings)
{
	auto auxiliaryNode = gapTree.get_child_optional(L"Auxiliary");

	if (auxiliaryNode)
		try
		{
			return Auxiliary(auxiliaryNode.get());
		}
		catch (const std::exception& e)
		{
			warnings.add("Failed to load auxiliary info from json. Falling back to API. (%s)", e.what());
		}

	return wrapper.getAuxiliary();
}

}

GDALDataset* Dataset::Open(GDALOpenInfo* openInfo)
{
	if (openInfo->pszFilename == nullptr || !openInfo->bStatOK)
		return nullptr;

	const bfs::path path = openInfo->pszFilename;
	const auto lowerExtension = boost::to_lower_copy(path.extension().wstring());
	if (lowerExtension != L".gap")
		return nullptr;

	Warnings warnings;
	WarningsReporter warningsReporter(warnings);
	WarningsContext context(warnings, bfs::absolute(path).string() + ": ");

	try
	{
		auto gapTree = loadJson(path);

		const auto apiParams = ApiParams(gapTree.get_child(L"EnterprisePredRasterApi"), path);
		auto apiWrapper = std::make_shared<ApiWrapper>(apiParams);

		if (openInfo->eAccess != GA_ReadOnly)
		{
			CPLError(CE_Failure, CPLE_NotSupported, "The Aircom ENTERPRISE Prediction driver only supports readonly access to existing datasets.\n");
			return nullptr;
		}

		autoCompleteAuxiliary(gapTree, path, *apiWrapper);

		auto ds = std::make_unique<Dataset>(gapTree, std::move(apiWrapper), warnings);
		ds->SetDescription(openInfo->pszFilename);
		ds->oOvManager.Initialize(ds.get(), openInfo->pszFilename);
		return ds.release();
	}
	catch (const boost::property_tree::json_parser_error&)
	{
		warnings.add("File has a .gap extension but is no valid JSON file, so not suited for Aircom ENTERPRISE Prediction driver");
	}
	catch (const std::exception& e)
	{
		warnings.add("File has a .gap extension but Aircom ENTERPRISE Prediction driver failed to load it: %s", e.what());
	}

	return nullptr;
}

void Dataset::autoCompleteAuxiliary(wptree& gapTree, const bfs::path& path, ApiWrapper& apiWrapper)
{
	auto& auxiliaryNode = gapTree.get_child_optional(L"Auxiliary");
	if (auxiliaryNode && boost::to_lower_copy(auxiliaryNode->data()) == L"autocomplete")
	{
		auxiliaryNode.get().swap(apiWrapper.getAuxiliary().asPropertyTree());
		write_json(path.string(), gapTree);
	}
}

Dataset::Dataset(const wptree& gapTree, std::shared_ptr<ApiWrapper> tmpApiWrapper, Warnings& warnings)
	: apiWrapper(std::move(tmpApiWrapper))
	, auxiliary(parseOrLoadAuxiliary(gapTree, *apiWrapper, warnings))
{
	setBoundingBox();

	if (auto meta = gapTree.get_child_optional(L"Meta"))
	{
		for (auto& metaDomain : meta.get())
		{
			auto domain = ws2ns(metaDomain.first);
			for (auto& kv : metaDomain.second)
			{
				auto key = ws2ns(kv.first);
				auto value = ws2ns(kv.second.data());
				SetMetadataItem(key.c_str(), value.c_str(), domain.c_str());
			}
		}
	}

	const auto requestedSection = apiWrapper->getParams().section;
	const Point sizeInPixels = { nRasterXSize, nRasterYSize };

	for (const auto& sectionPair : auxiliary.sectionInfos)
	{
		const auto sectionNum = sectionPair.first;
		const int bandIndex = sectionNum + 1;
		if (requestedSection == Section::Unspecified || static_cast<int>(requestedSection) == sectionNum)
			SetBand(bandIndex, new RasterBand(this, sizeInPixels, bandIndex, apiWrapper, sectionNum, sectionPair.second));
	}
}

CPLErr Dataset::GetGeoTransform(double* padfTransform)
{
	if (!padfTransform)
		return CPLErr::CE_Failure;

	const double res = getResolution();

	padfTransform[0] = getBoundingBox().min_corner().x(); // min x
	padfTransform[1] = res;
	padfTransform[2] = 0;
	// top-down
	padfTransform[3] = getBoundingBox().max_corner().y(); // max y
	padfTransform[4] = 0;
	padfTransform[5] = -res;

	return CPLErr::CE_None;
}

const char* Dataset::GetProjectionRef()
{
	// getting the projection based on EPSG codes is expensive, so cache them
	static std::mutex mutex;
	static std::map<int, std::string> cachedProjections;

	const int epsg = auxiliary.epsg;
	if (epsg == 0)
		return "";

	std::lock_guard<std::mutex> lock(mutex);
	auto it = cachedProjections.find(epsg);
	if (it != cachedProjections.end())
		return it->second.c_str();

	OGRSpatialReference spatialRef;
	if (spatialRef.importFromEPSG(epsg) != OGRERR_NONE)
		throw std::runtime_error(format("Dataset::GetProjectionRef(): unsupported EPSG code %d", epsg));

	char* rawWktString = nullptr;
	spatialRef.exportToWkt(&rawWktString);
	std::string wktString = rawWktString;
	OGRFree(rawWktString);

	cachedProjections.emplace(epsg, wktString);

	return wktString.c_str();
}

void Dataset::setBoundingBox()
{
	const double res = getResolution();
	nRasterXSize = static_cast<int>(std::ceil(width(getBoundingBox()) / res));
	nRasterYSize = static_cast<int>(std::ceil(height(getBoundingBox()) / res));

	if (nRasterXSize <= 0 || nRasterYSize <= 0)
		throw std::runtime_error(format("Invalid dimensions: %d x %d", nRasterXSize, nRasterYSize));
}

}}
