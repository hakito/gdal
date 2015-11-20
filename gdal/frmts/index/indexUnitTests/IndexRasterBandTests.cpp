#include "IndexRasterBand.h"
#include "IndexBlocksBuilder.h"

#include <gmock/gmock.h>

TEST(IndexRasterBand, constructor)
{
	IndexDataset dataset(IndexBlocks(), nullptr, "");
	IndexRasterBand band(&dataset, 666);

	EXPECT_EQ(666, band.GetBand());
	EXPECT_EQ(GDALDataType::GDT_Int16, band.GetRasterDataType());
	EXPECT_EQ(-9999, band.GetNoDataValue());

	EXPECT_EQ(GDALColorInterp::GCI_GrayIndex, band.GetColorInterpretation());
}

TEST(IndexRasterBand, clutterNames)
{
	IndexDataset heightDataset(IndexBlocks(), nullptr, "");
	IndexRasterBand heightBand(&heightDataset);

	EXPECT_EQ(nullptr, heightBand.GetCategoryNames());

	auto clutterFile = std::make_unique<std::stringstream>();
	*clutterFile << "0 sea\n1 rural\n";

	IndexDataset clutterDataset(IndexBlocks(), std::move(clutterFile), "");
	IndexRasterBand clutterBand(&clutterDataset);
	auto** names = clutterBand.GetCategoryNames();

	ASSERT_NE(nullptr, names);
	EXPECT_EQ("sea", std::string(names[0]));
	EXPECT_EQ("rural", std::string(names[1]));
	EXPECT_EQ(nullptr, names[2]);
}

TEST(IndexRasterBand, rasterIO)
{
	IndexBlocksBuilder builder;
	builder.addBlock().from(0, 0).to(4, 4).resolution(2).withData(
		{ 0, 1, // top-down!
		  2, 3 });
	builder.addBlock().from(2, 0).to(4, 2).resolution(1).withData(
		{ 10, 12,
		  14, 16 });

	IndexDataset dataset(builder.create(), nullptr, "");
	auto& band = *dataset.GetRasterBand(1);

	auto readPixels = [&band](const MapBox& sourceRegion, int widthInPixels, int heightInPixels,
		GDALRIOResampleAlg algorithm)
	{
		std::vector<std::int16_t> pixels(static_cast<size_t>(widthInPixels) * heightInPixels);

		GDALRasterIOExtraArg extraArg = { 0 };
		extraArg.nVersion = RASTERIO_EXTRA_ARG_CURRENT_VERSION;
		extraArg.eResampleAlg = algorithm;

		auto error = band.RasterIO(GDALRWFlag::GF_Read,
			sourceRegion.min_corner().get<0>(), sourceRegion.min_corner().get<1>(),
			width(sourceRegion), height(sourceRegion),
			pixels.data(), widthInPixels, heightInPixels, GDALDataType::GDT_Int16,
			0, 0,
			&extraArg);
		EXPECT_EQ(CPLErr::CE_None, error);

		return pixels;
	};

	// whole bounding box with resolution = 2
	auto pixels = readPixels(makeBox(0, 0, 4, 4), 2, 2, GDALRIOResampleAlg::GRIORA_Bilinear);
	EXPECT_THAT(pixels, testing::ElementsAre(
		2, 13, // bottom-up
		0, 1));

	// a region with resolution = 1
	pixels = readPixels(makeBox(2, 1, 4, 4), 2, 3, GDALRIOResampleAlg::GRIORA_NearestNeighbour);
	EXPECT_THAT(pixels, testing::ElementsAre(
		 10, 12,
		  1,  1,
		  1,  1));
}
