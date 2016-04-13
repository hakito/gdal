#pragma once

#include "gdal_pam.h"

#include "Dataset.h"

namespace aircom { namespace pred_raster {

class RasterBand : public GDALRasterBand
{
	std::shared_ptr<ApiWrapper> apiWrapper;
	const unsigned long sectionNum;
	boost::optional<double> noDataValue;

public:
	RasterBand(Dataset* owningDataSet, MapPoint sizeInPixels,
		int bandIndex, std::shared_ptr<ApiWrapper> apiWrapper,
		unsigned long sectionNum, const Auxiliary& sectionInfo);

	virtual GDALColorInterp GetColorInterpretation() override;

	virtual CPLErr SetNoDataValue(double value) override;
	virtual double GetNoDataValue(int* pbSuccess = nullptr) override;

protected:
	virtual CPLErr IReadBlock(int nXBlockOff, int nYBlockOff, void* pImage) override;

private:
	IPredRaster5Ptr getPredRaster() { return apiWrapper->getPredRaster(); }

	size_t getNumPixelsPerBlock() const { return size_t(nBlockXSize) * nBlockYSize; }

	void readTile(IRasterTilePtr tile, size_t numPixels, void* data) const;

	void fillNoDataBlock(void* blockData) const;
	void fillPartialBlock(IRasterTilePtr tile, void* blockData) const;
};

}}
