#include "SAMdisk.h"
#include "Sector.h"

Sector::Sector (DataRate datarate_, Encoding encoding_, const Header &header_, int gap3_)
	: header(header_), datarate(datarate_), encoding(encoding_), gap3(gap3_)
{
}

bool Sector::operator== (const Sector &sector) const
{
	// Headers must match
	if (sector.header != header)
		return false;

	// If neither has data it's a match
	if (sector.m_data.size() == 0 && m_data.size() == 0)
		return true;

	// Both sectors must have some data
	if (sector.copies() == 0 || copies() == 0)
		return false;

	// Both first sectors must have at least the natural size to compare
	if (sector.data_size() < sector.size() || data_size() < size())
		return false;

	// The natural data contents must match
	return std::equal(data_copy().begin(), data_copy().begin() + size(), sector.data_copy().begin());
}

int Sector::size () const
{
	return header.sector_size();
}

int Sector::data_size () const
{
	return copies() ? static_cast<int>(m_data[0].size()) : 0;
}

const DataList &Sector::datas () const
{
	return m_data;
}

DataList &Sector::datas ()
{
	return m_data;
}

const Data &Sector::data_copy (int copy/*=0*/) const
{
	copy = std::max(std::min(copy, static_cast<int>(m_data.size()) - 1), 0);
	return m_data[copy];
}

Data &Sector::data_copy (int copy/*=0*/)
{
	assert(m_data.size() != 0);
	copy = std::max(std::min(copy, static_cast<int>(m_data.size()) - 1), 0);
	return m_data[copy];
}

int Sector::copies () const
{
	return static_cast<int>(m_data.size());
}

Sector::Merge Sector::add (Data &&data, bool bad_crc, uint8_t new_dam)
{
	Merge ret = Merge::NewData;
	assert(!copies() || dam == new_dam);

	// If the sector has a bad header CRC, it can't have any data
	if (has_badidcrc())
		return Merge::Unchanged;

	// If there's enough data, check the CRC state
	if (static_cast<int>(data.size()) >= (size() + 2))
	{
		CRC16 crc;
		if (encoding == Encoding::MFM) crc.init(CRC16::A1A1A1);
		crc.add(new_dam);
		auto bad_data_crc = crc.add(data.data(), size() + 2) != 0;
		assert(bad_crc == bad_data_crc);
		(void)bad_data_crc;
	}

	// If the exising sector has good data, ignore supplied data if it's bad
	if (bad_crc && copies() && !has_baddatacrc())
		return Merge::Unchanged;

	// If the existing sector is bad, new good data will replace it all
	if (!bad_crc && has_baddatacrc())
	{
		remove_data();
		ret = Merge::Improved;
	}

	// 8K sectors always have a CRC error, but may include a secondary checksum
	if (is_8k_sector())
	{
		// Attempt to identify the 8K checksum method used by the new data
		auto chk8k_method = Get8KChecksumMethod(data.data(), data.size());

		// If it's recognised, replace any existing data with it
		if (chk8k_method >= CHK8K_FOUND)
		{
			remove_data();
			ret = Merge::Improved;
		}
		// Do we already have a copy?
		else if (copies() == 1)
		{
			// Can we identify the method used by the existing copy?
			chk8k_method = Get8KChecksumMethod(m_data[0].data(), m_data[0].size());
			if (chk8k_method >= CHK8K_FOUND)
			{
				// Keep the existing, ignoring the new data
				return Merge::Unchanged;
			}
		}
	}

	// Look for existing data that is a superset of what we're adding
	auto it = std::find_if(m_data.begin(), m_data.end(), [&] (const Data &d) {
		return d.size() >= data.size() && std::equal(data.begin(), data.end(), d.begin());
	});

	// Return if we already have a better copy
	if (it != m_data.end())
		return Merge::Unchanged;

	// Look for existing data that is a subset of what we're adding
	it = std::find_if(m_data.begin(), m_data.end(), [&] (const Data &d) {
		return d.size() <= data.size() && std::equal(d.begin(), d.end(), data.begin());
	});

	// Remove the inferior copy
	if (it != m_data.end())
	{
		m_data.erase(it);
		ret = Merge::Improved;
	}

	// DD 8K sectors are considered complete at 6K, everything else at natural size
	auto complete_size = is_8k_sector() ? 0x1800 : data.size();

	// Is the supplied data enough for a complete sector?
	if (data.size() >= complete_size)
	{
		// Look for existing data that contains the same normal sector data
		it = std::find_if(m_data.begin(), m_data.end(), [&] (const Data &d) {
			return d.size() >= complete_size && std::equal(d.begin(), d.begin() + complete_size, data.begin());
		});

		// Found a match?
		if (it != m_data.end())
		{
			// Return if the new one isn't larger
			if (data.size() <= it->size())
				return Merge::Unchanged;

			// Remove the existing smaller copy
			m_data.erase(it);
		}

		// Will we now have multiple copies?
		if (m_data.size() > 0)
		{
			// Keep multiple copies the same size, whichever is shortest
			auto new_size = std::min(data.size(), m_data[0].size());
			data.resize(new_size);

			// Resize any existing copies to match
			for (auto &d : m_data)
				d.resize(new_size);
		}
	}

	// Insert the new data copy, unless it the copy count (default is 3)
	if (copies() < opt.maxcopies)
		m_data.emplace_back(std::move(data));

	// Update the data CRC state and DAM
	m_bad_data_crc = bad_crc;
	dam = new_dam;

	return ret;
}

Sector::Merge Sector::merge (Sector &&sector)
{
	Merge ret = Merge::Unchanged;

	// If the new header CRC is bad there's nothing we can use
	if (sector.has_badidcrc())
		return Merge::Unchanged;

	// Something is wrong if the new details don't match the existing one
	assert(sector.header == header);
	assert(sector.datarate == datarate);
	assert(sector.encoding == encoding);

	// If the existing header is bad, repair it
	if (has_badidcrc())
	{
		header = sector.header;
		set_badidcrc(false);
		ret = Merge::Improved;
	}

	// We can't repair good data with bad
	if (!has_baddatacrc() && sector.has_baddatacrc())
		return ret;

	// Add the new data snapshots
	for (Data &data : sector.m_data)
	{
		// Move the data into place, passing on the existing data CRC status and DAM
		if (add(std::move(data), sector.has_baddatacrc(), sector.dam) != Merge::Unchanged)
			ret = Merge::Improved;	// ToDo: detect NewData return?
	}
	sector.m_data.clear();

	return ret;
}


bool Sector::has_data () const
{
	return copies() != 0;
}

bool Sector::has_gapdata () const
{
	return data_size() > size();
}

bool Sector::has_shortdata () const
{
	return data_size() < size();
}

bool Sector::has_badidcrc () const
{
	return m_bad_id_crc;
}

bool Sector::has_baddatacrc () const
{
	return m_bad_data_crc;
}

bool Sector::is_deleted () const
{
	return dam == 0xf8 || dam == 0xf9;
}

bool Sector::is_altdam () const
{
	return dam == 0xfa;
}

bool Sector::is_rx02dam () const
{
	return dam == 0xfd;
}

bool Sector::is_8k_sector () const
{
	// +3 and CPC disks treat this as a virtual complete sector
	return datarate == DataRate::_250K && encoding == Encoding::MFM && header.size == 6;
}

void Sector::set_badidcrc (bool bad)
{
	m_bad_id_crc = bad;

	if (bad)
		remove_data();
}

void Sector::set_baddatacrc (bool bad)
{
	m_bad_data_crc = bad;

	if (!bad && copies() > 1)
		m_data.resize(1);
}

void Sector::remove_data ()
{
	m_data.clear();
	m_bad_data_crc = false;
	dam = 0xfb;
}

void Sector::remove_gapdata ()
{
	if (!has_gapdata())
		return;

	for (auto &data : m_data)
		data.resize(size());
}

// Map a size code to how it's treated by the uPD765 FDC on the PC
int Sector::SizeCodeToRealSizeCode (int size)
{
	// Sizes above 8 are treated as 8 (32K)
	return (size <= 7) ? size : 8;
}

// Return the sector length for a given sector size code
int Sector::SizeCodeToLength (int size)
{
	// 2 ^ (7 + size)
	return 128 << SizeCodeToRealSizeCode(size);
}
