#include "RasterBand.h"

namespace aircom { namespace pred_raster {

namespace {

boost::optional<double> getNoDataValue(const SectionInfo& sectionInfo)
{
	switch (sectionInfo.dataType)
	{
	case GDT_Float32:
		return -9999; // at least for unmasked pathloss according to Aircom docs, but apparently 200 in practice
	case GDT_Int16:
		return -9999; // at least for unmasked angles, but potentially 0 in practice
	case GDT_Byte:
		return 200;   // at least for masked loss and masked+unmasked LineOfSightInfo
	}

	return boost::none;
}

bool getValidValuesRange(const unsigned long sectionNum, double& minValue, double& maxValue)
{
	switch (sectionNum)
	{
	case 0: // pathloss
		minValue = 0;
		maxValue = 200;
		return true;
	case 1: // angle
		minValue = -18000; // -180�
		maxValue =  18000; // +180�
		return true;
	default:
		return false;
	}
}

template <typename T>
void fillWithNoDataValue(const T noDataValue, void* const data, const size_t numPixels)
{
	T* typedData = static_cast<T*>(data);
	for (size_t i = 0; i < numPixels; ++i)
		typedData[i] = noDataValue;
}

template <typename T>
void postProcessBlockRow(void* const data, const int widthInPixels,
	const RasterBand::RowSegment blockRowSegmentInsidePredictionRadius,
	const boost::optional<double> noDataValue, const double minValue, const double maxValue)
{
	T* const typedData = static_cast<T*>(data);
	const T typedMinValue = static_cast<T>(minValue);
	const T typedMaxValue = static_cast<T>(maxValue);

	T typedNoDataValue;
	if (noDataValue)
		typedNoDataValue = static_cast<T>(*noDataValue);

	if (noDataValue)
	{
		for (int x = 0; x < blockRowSegmentInsidePredictionRadius.start; ++x)
			typedData[x] = typedNoDataValue;
	}

	for (int x = blockRowSegmentInsidePredictionRadius.start; x < blockRowSegmentInsidePredictionRadius.end; ++x)
	{
		T& value = typedData[x];

		// skip existing no-data values
		if (noDataValue && value == typedNoDataValue)
			continue;

		if (value < typedMinValue)
			value = typedMinValue;
		else if (value > typedMaxValue)
			value = typedMaxValue;
	}

	if (noDataValue)
	{
		for (int x = blockRowSegmentInsidePredictionRadius.end; x < widthInPixels; ++x)
			typedData[x] = typedNoDataValue;
	}
}

}

RasterBand::RasterBand(Dataset* owningDataSet, MapPoint sizeInPixels,
	int bandIndex, std::shared_ptr<ApiWrapper> tmpApiWrapper,
	unsigned long sectionNum, const SectionInfo& sectionInfo)
	: apiWrapper(std::move(tmpApiWrapper))
	, sectionNum(sectionNum)
	, noDataValue(getNoDataValue(sectionInfo))
{
	poDS = owningDataSet;
	nBand = bandIndex;
	eDataType = sectionInfo.dataType;

	nRasterXSize = sizeInPixels.get<0>();
	nRasterYSize = sizeInPixels.get<1>();

	nBlockXSize = sectionInfo.tileSizeInPixels.get<0>();
	nBlockYSize = sectionInfo.tileSizeInPixels.get<1>();

	nBlocksPerRow = DIV_ROUND_UP(nRasterXSize, nBlockXSize);
	nBlocksPerColumn = DIV_ROUND_UP(nRasterYSize, nBlockYSize);
}

GDALColorInterp RasterBand::GetColorInterpretation()
{
	return GDALColorInterp::GCI_GrayIndex;
}

CPLErr RasterBand::SetNoDataValue(double value)
{
	noDataValue = value;
	return CPLErr::CE_None;
}

double RasterBand::GetNoDataValue(int* pbSuccess)
{
	if (pbSuccess)
		*pbSuccess = (noDataValue ? TRUE : FALSE);

	return noDataValue.get_value_or(std::numeric_limits<double>::quiet_NaN());
}

CPLErr RasterBand::IReadBlock(int nXBlockOff, int nYBlockOff, void* pImage)
{
	try
	{
		const MapPoint blockIndex = { nXBlockOff, nYBlockOff };
		auto tileIterator = getPredRaster()->CreateTileIterator(sectionNum);

		if (readBlock(tileIterator, blockIndex, pImage))
			postProcessBlock(blockIndex, pImage);
	}
	catch (const std::exception& e)
	{
		CPLError(CE_Failure, CPLE_AppDefined, e.what());
		return CE_Failure;
	}

	return CPLErr::CE_None;
}

// Returns false if there's no tile for this block, i.e., if it only contains no-data values.
bool RasterBand::readBlock(IPredRasterTileIteratorPtr tileIterator, MapPoint blockIndex, void* data) const
{
	IRasterTilePtr tile;
	if (FAILED(tileIterator->raw_GetTile(blockIndex.get<0>(), blockIndex.get<1>(), &tile)))
	{
		// missing tile
		fillNoDataBlock(data);
		return false;
	}

	const size_t numTilePixels = tile->GetPixelCount();
	const size_t numBlockPixels = getNumPixelsPerBlock();

	if (numTilePixels == numBlockPixels)
	{
		// full tile
		readTile(tile, numBlockPixels, data);
	}
	else
	{
		// the tile is smaller than the GDAL block
		fillPartialBlock(tile, data);
	}

	return true;
}

void RasterBand::readTile(IRasterTilePtr tile, size_t numPixels, void* data) const
{
	const auto numPixelsLong = static_cast<unsigned long>(numPixels);

	switch (eDataType)
	{
	case GDT_Byte:
		tile->GetUCharData(numPixelsLong, static_cast<unsigned char*>(data));
		break;
	case GDT_Int16:
		tile->GetShortData(numPixelsLong, static_cast<short*>(data));
		break;
	case GDT_UInt16:
		tile->GetUShortData(numPixelsLong, static_cast<unsigned short*>(data));
		break;
	case GDT_Int32:
		tile->GetIntData(numPixelsLong, static_cast<int*>(data));
		break;
	case GDT_UInt32:
		tile->GetUIntData(numPixelsLong, static_cast<unsigned int*>(data));
		break;
	case GDT_Float32:
		tile->GetFloatData(numPixelsLong, static_cast<float*>(data));
		break;
	case GDT_Float64:
		tile->GetDoubleData(numPixelsLong, static_cast<double*>(data));
		break;
	default:
		throw std::logic_error("RasterBand::readTile(): unsupported GDAL data type!");
	}
}

void RasterBand::fillNoDataBlock(void* blockData) const
{
	if (!noDataValue)
		throw std::runtime_error("RasterBand::fillNoDataBlock(): unknown no-data value");

	const size_t numPixels = getNumPixelsPerBlock();

	switch (eDataType)
	{
	case GDT_Byte:
		fillWithNoDataValue(static_cast<std::uint8_t>(*noDataValue), blockData, numPixels);
		break;
	case GDT_Int16:
		fillWithNoDataValue(static_cast<std::int16_t>(*noDataValue), blockData, numPixels);
		break;
	case GDT_UInt16:
		fillWithNoDataValue(static_cast<std::uint16_t>(*noDataValue), blockData, numPixels);
		break;
	case GDT_Int32:
		fillWithNoDataValue(static_cast<std::int32_t>(*noDataValue), blockData, numPixels);
		break;
	case GDT_UInt32:
		fillWithNoDataValue(static_cast<std::uint32_t>(*noDataValue), blockData, numPixels);
		break;
	case GDT_Float32:
		fillWithNoDataValue(static_cast<float>(*noDataValue), blockData, numPixels);
		break;
	case GDT_Float64:
		fillWithNoDataValue(*noDataValue, blockData, numPixels);
		break;
	}
}

void RasterBand::fillPartialBlock(IRasterTilePtr tile, void* blockData) const
{
	_REGIONEX tileRegion;
	tile->GetTileRegion(&tileRegion);

	if (tileRegion.m_width > nBlockXSize || tileRegion.m_height > nBlockYSize)
		throw std::logic_error("RasterBand::fillPartialBlock(): a tile dimension is larger than the block's!");

	const size_t numTilePixels = size_t(tileRegion.m_width) * tileRegion.m_height;
	const size_t bytesPerPixel = GDALGetDataTypeSize(eDataType) / 8;

	auto tileData = std::make_unique<char[]>(numTilePixels * bytesPerPixel);
	readTile(tile, numTilePixels, tileData.get());

	for (int y = 0; y < tileRegion.m_height; ++y)
	{
		const void* tileRow = tileData.get() + (y * tileRegion.m_width * bytesPerPixel);
		void* blockRow = static_cast<char*>(blockData) + (y * nBlockXSize * bytesPerPixel);
		memcpy(blockRow, tileRow, tileRegion.m_width * bytesPerPixel);
	}
}

void RasterBand::postProcessBlock(MapPoint blockIndex, void* data)
{
	if (rowSegmentsInsidePredictionRadius.empty())
		computeRowSegmentsInsidePredictionRadius();

	const int startColumnIndex = blockIndex.get<0>() * nBlockXSize;
	const int startRowIndex = blockIndex.get<1>() * nBlockYSize;

	char* const charData = static_cast<char*>(data);
	const size_t blockRowSize = size_t(nBlockXSize) * (GDALGetDataTypeSize(eDataType) / 8);

	const auto computeBlockRowSegment = [&, this](int y) -> RowSegment
	{
		const auto& rowSegment = rowSegmentsInsidePredictionRadius[startRowIndex + y];

		// make the row segment relative to the block and clip it
		return {
			std::max(0, std::min(nBlockXSize, rowSegment.start - startColumnIndex)),
			std::max(0, std::min(nBlockXSize, rowSegment.end - startColumnIndex))
		};
	};

	double minValue = std::numeric_limits<double>::quiet_NaN();
	double maxValue = minValue;
	getValidValuesRange(sectionNum, minValue, maxValue);

	switch (eDataType)
	{
	case GDT_Byte:
		for (int y = 0; y < nBlockYSize; ++y)
			postProcessBlockRow<std::uint8_t>(charData + y * blockRowSize, nBlockXSize, computeBlockRowSegment(y), noDataValue, minValue, maxValue);
		break;
	case GDT_Int16:
		for (int y = 0; y < nBlockYSize; ++y)
			postProcessBlockRow<std::int16_t>(charData + y * blockRowSize, nBlockXSize, computeBlockRowSegment(y), noDataValue, minValue, maxValue);
		break;
	case GDT_UInt16:
		for (int y = 0; y < nBlockYSize; ++y)
			postProcessBlockRow<std::uint16_t>(charData + y * blockRowSize, nBlockXSize, computeBlockRowSegment(y), noDataValue, minValue, maxValue);
		break;
	case GDT_Int32:
		for (int y = 0; y < nBlockYSize; ++y)
			postProcessBlockRow<std::int32_t>(charData + y * blockRowSize, nBlockXSize, computeBlockRowSegment(y), noDataValue, minValue, maxValue);
		break;
	case GDT_UInt32:
		for (int y = 0; y < nBlockYSize; ++y)
			postProcessBlockRow<std::uint32_t>(charData + y * blockRowSize, nBlockXSize, computeBlockRowSegment(y), noDataValue, minValue, maxValue);
		break;
	case GDT_Float32:
		for (int y = 0; y < nBlockYSize; ++y)
			postProcessBlockRow<float>(charData + y * blockRowSize, nBlockXSize, computeBlockRowSegment(y), noDataValue, minValue, maxValue);
		break;
	case GDT_Float64:
		for (int y = 0; y < nBlockYSize; ++y)
			postProcessBlockRow<double>(charData + y * blockRowSize, nBlockXSize, computeBlockRowSegment(y), noDataValue, minValue, maxValue);
		break;
	}
}

void RasterBand::computeRowSegmentsInsidePredictionRadius()
{
	const auto& predData = apiWrapper->getParams().predData;
	const auto& boundingBox = apiWrapper->getAuxiliary().boundingBox;

	const double res = predData.nResolution_cm / 100.0;
	const double radius = predData.nRadius_cm / 100.0;
	const double radiusSquared = radius * radius;

	const double txX = predData.nX_cm / 100.0;
	const double txY = predData.nY_cm / 100.0;

	const double leftmostPixelCenter = boundingBox.min_corner().get<0>() + 0.5 * res;
	const double topmostPixelCenter = boundingBox.max_corner().get<1>() - 0.5 * res;

	rowSegmentsInsidePredictionRadius.resize(nRasterYSize);
	for (int rowIndex = 0; rowIndex < nRasterYSize; ++rowIndex)
	{
		auto& rowSegment = rowSegmentsInsidePredictionRadius[rowIndex];

		const double y = topmostPixelCenter - rowIndex * res;
		const double yDistance = std::abs(y - txY);

		if (yDistance > radius)
		{
			rowSegment = { 0, 0 };
			continue;
		}

		const double xDistance = std::sqrt(radiusSquared - yDistance*yDistance);
		const double segmentStart = txX - xDistance;
		const double segmentEnd = txX + xDistance;

		rowSegment = {
			int(std::ceil((segmentStart - leftmostPixelCenter) / res)),   // leftmost pixel whose center is inside the exact segment
			int(std::floor((segmentEnd - leftmostPixelCenter) / res)) + 1 // pixel to the right of the rightmost pixel whose center is inside the exact segment
		};
	}
}

}}
