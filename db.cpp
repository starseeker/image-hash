
#include "db.h"
#include "SQLiteCpp/SQLiteCpp.h"

namespace imghash {
	class Database::Impl {
	protected:
		using stmt_ptr = std::unique_ptr<SQLite::Statement>;

		/* String constants */
		static constexpr const char str_init_tables[] = {
			//table sizes
			"CREATE TABLE IF NOT EXISTS mvp_counts ("
				"id INTEGER PRIMARY KEY,"
				"points INTEGER,"
				"vantange_points INTEGER,"
				"parts INTEGER,"
				"items INTEGER"
			");"

			"CREATE TABLE IF NOT EXISTS mvp_points ("
				"id INTEGER PRIMARY KEY,"
				"part INTEGER," // NB: not an index into mvp_parts, but rather computed from the data in that table
				"value BLOB UNIQUE"
			// "d0 INTEGER," etc are added later for each vantage_point with an ALTER TABLE
			");"
			"CREATE INDEX IF NOT EXISTS mvp_idx_points_part ON mvp_points(part);"

			"CREATE TABLE IF NOT EXISTS mvp_vantage_points ("
				"id INTEGER PRIMARY KEY,"
				"value BLOB UNIQUE" // not necessarily in mvp_points
			");"

			"CREATE TABLE IF NOT EXISTS mvp_parts ("
				"id INTEGER PRIMARY KEY,"
				"vantage_point_id INTEGER,"
				"upper_bound INTEGER,"
				"count INTEGER," // how many points are within this partition shell (excluding the lower shells)
				"FOREIGN KEY(vantage_point_id) REFERENCES mvp_vantage_points(id)"
			");"

			"CREATE TABLE temp.mvp_query ("
				"id INTEGER PRIMARY KEY,"
				"dist INTEGER" //distance to query point
			");"
			"CREATE INDEX temp.mvp_idx_query_dist ON temp.mvp_query(dist);"

			"CREATE TABLE IF NOT EXISTS mvp_items ("
				"id INTEGER PRIMARY KEY," //to index into another table holding item data
				"point_id INTEGER," //multiple items may be associated with the same point
				"FOREIGN KEY(point_id) REFERENCES mvp_points(id)"
			");"
			"CREATE INDEX IF NOT EXISTS mvp_idx_items_point ON mvp_items(point_id);"
		};

		static constexpr const char str_count_rows[] = 
			"SELECT COUNT(1) FROM $table;";
		
		static constexpr const char str_ins_counts[] = 
			"INSERT INTO mvp_counts(points,vantage_points,parts,items) VALUES($points,$vantage_points,$parts,$items)";
		
		static constexpr const char str_upd_count[] =
			"UPDATE mvp_counts SET $col = $col + 1 WHERE id = 1;";

		static constexpr const char str_sel_all_points[] = 
			"SELECT id, value FROM mvp_points;";
		
		static constexpr const char str_sel_point_by_value[] = 
			"SELECT id FROM mvp_points WHERE value = $value;";
		
		static constexpr const char str_upd_point[] = 
			"UPDATE mvp_points SET $col = $value WHERE id = $id;";
		
		static constexpr const char str_add_points_col[] =
			"ALTER TABLE mvp_points ADD COLUMN $col INTEGER DEFAULT 0x7FFFFFFF;" // max int32
			"CREATE INDEX $idx ON mvp_points($col);";
		
		static constexpr const char str_sel_vp_ids[] = 
			"SELECT id FROM mvp_vantage_points ORDER BY id ASC;";
		
		static constexpr const char str_sel_vps[] = 
			"SELECT id, value FROM mvp_vantage_points ORDER BY id ASC;";
		
		static constexpr const char str_ins_vp[] = 
			"INSERT INTO mvp_vantage_points(value) VALUES($value) RETURNING id;";
		
		static constexpr const char str_sel_parts[] = 
			"SELECT vantage_point_id, upper_bound FROM mvp_parts ORDER BY vantage_point_id ASC, upper_bound ASC;";
		
		static constexpr const char str_del_query[] =
			"DELETE FROM temp.mvp_query;";
		
		static constexpr const char str_ins_query[] =
			"INSERT INTO temp.mvp_query(id, dist) VALUES($id, $dist);";

		static constexpr const char str_ins_item[] =
			"INSERT INTO mvp_items(point_id) VALUES($point_id) RETURNING id;";

		static constexpr const char str_upd_item[] =
			"UPDATE mvp_items SET point_id = $point_id WHERE id = $id;";

		static constexpr const char str_sel_item_by_id[] =
			"SELECT point_id FROM items WHERE id = $id;";

		/* Cached compiled SQL statememts */
		stmt_ptr count_rows; // $table: $table -> count
		stmt_ptr ins_counts; // mvp_counts: $points,$vantage_points,$parts,$items ->
		stmt_ptr upd_count; // mvp_counts: $col ->
		stmt_ptr sel_all_points; // mvp_points: -> id, value
		stmt_ptr sel_point_by_value; // mvp_points: $value -> id
		stmt_ptr ins_point; // mvp_points: $part, $value, $d0, ... -> id
		stmt_ptr upd_point; // mvp_points: $col, $value, $id ->
		stmt_ptr add_points_col; // mvp_points: $col, $idx ->
		stmt_ptr sel_vp_ids; // mvp_vantage_points: -> id
		stmt_ptr sel_vps; // mvp_vantage_points: -> id, value
		stmt_ptr ins_vp; // mvp_vantage_points: $value -> id
		stmt_ptr sel_parts; // mvp_parts: -> vantage_point_id, upper_bound
		stmt_ptr del_query; // mvp_query: ->
		stmt_ptr ins_query; // mvp_query: $id, $dist ->
		stmt_ptr ins_item; // mvp_items: $point_id -> id
		stmt_ptr upd_item; // mvp_items: $point_id, $id ->
		stmt_ptr sel_item_by_id; // mvp_items: $id -> point_id

		/* SQL BLOB <-> point_type interface*/
		
		// Convert the value BLOB into point_type
		static point_type get_point_value(SQLite::Column& col) {
			size_t n = col.getBytes();
			const uint8_t* data = static_cast<const uint8_t*>(col.getBlob());
			return point_type(data, data + n);
		}

		// Convert point_type into a BLOB
		static const void* get_point_data(const point_type& p) {
			return p.data();
		}
		static int get_point_size(const point_type& p) {
			return static_cast<int>(p.size());
		}

		// Get the distance between two points
		static int get_distance(const point_type& p1, const point_type& p2)
		{
			return static_cast<int>(Hash::distance(p1, p2)); //TODO: check distance < max int
		}

		//The database connection
		SQLite::Database db;
		
		// Cached vantage-point IDs, for insert_point 
		std::vector<int64_t> vp_ids_;

		stmt_ptr make_stmt(const std::string stmt)
		{
			return std::make_unique<SQLite::Statement>(db, stmt);
		}

		//update cached vp_ids
		// deletes insert_point and parition_points if it changes
		void update_vp_ids(const std::vector<int64_t>& vp_ids)
		{
			if (!std::equal(vp_ids_.begin(), vp_ids_.end(), vp_ids.begin(), vp_ids.end())) {
				vp_ids_ = vp_ids;
				insert_point.reset();
				partition_points.reset();
			}
		}

		// INSERT INTO points(value, d0, d1, ...) VALUES (?, ?, ?, ...) RETURNING id;
		// Where d0, d1, ... are "d{id}" for id in vp_ids
		stmt_ptr make_insert_point(const std::vector<int64_t>& vp_ids)
		{
			std::string stmt1 = "INSERT INTO points(value";
			std::string stmt2 = ") VALUES (?";
			for (int64_t id : vp_ids) {
				auto id_str = std::to_string(id);
				stmt1 += ", d" + id_str;
				stmt2 += ", ?";
			}
			stmt2 += ") RETURNING id;";
			return make_stmt(stmt1 + stmt2);
		}

		// SELECT id, value FROM points WHERE (d0 BETWEEN ? AND ?) AND (d1 BETWEEN ? AND ?) AND ...;
		// Where d0, d1, ... are "d{id}" for id in vp_ids
		stmt_ptr make_partition_points(const std::vector<int64_t>& vp_ids)
		{
			std::string stmt = "SELECT id, value FROM points";
			std::string pfx = " WHERE ";
			for (int64_t id : vp_ids) {
				stmt += pfx + "(d" + std::to_string(id) + " BETWEEN ? AND ?)";
				pfx = " AND ";
			}
			stmt += ";";
			return make_stmt(stmt);
		}

		int64_t do_count_rows(const std::string& table) {
			count_rows->bind("table", table);
			count_rows->reset();
			if (count_rows->executeStep()) {
				return count_rows->getColumn(0).getInt64();
			}
			else {
				throw std::runtime_error("Error counting rows of table: " + table);
			}
		}

		void init_tables()
		{
			SQLite::Transaction trans(db);
			db.exec(str_init_tables);
			
			count_rows = make_stmt(str_count_rows);
			ins_counts = make_stmt(str_ins_counts);
			sel_vp_ids = make_stmt(str_sel_vp_ids);

			//if counts is empty, initialize it
			if (do_count_rows("mvp_counts") == 0) {
				auto num_points = do_count_rows("mvp_points");
				auto num_vantage_points = do_count_rows("mvp_vantage_points");
				auto num_parts = do_count_rows("mvp_parts");
				auto num_items = do_count_rows("mvp_items");

				ins_counts->bind("points", num_points);
				ins_counts->bind("vantage_points", num_vantage_points);
				ins_counts->bind("parts", num_parts);
				ins_counts->bind("items", num_items);
				ins_counts->reset();
				ins_counts->exec();
			}
			
			//get all of the vantage point ids
			std::vector<int64_t> vp_ids;
			sel_vp_ids->reset();
			while (sel_vp_ids->executeStep()) {
				vp_ids.push_back(sel_vp_ids->getColumn(0).getInt64());
			}
			vp_ids_ = std::move(vp_ids);
			trans.commit();
		}

		//get the distance from each vantage point to the given point
		// calls update_vp_ids, which may invalidate certain statements
		std::vector<int32_t> vp_dists_(const point_type& p_value)
		{
			//Calculate distances to each vantage point
			std::vector<int64_t> vp_ids;
			std::vector<int32_t> dists;
			sel_vps->reset();
			while (sel_vps->executeStep()) {
				auto id = sel_vps->getColumn(0).getInt64(); //vantage point id
				vp_ids.push_back(id);

				auto vp_value = get_point_value(sel_vps->getColumn(1)); //vantage point value
				uint32_t d = get_distance(vp_value, p_value);
				dists.push_back(d);
			}
			update_vp_ids(vp_ids); //check to see if they changed since the last call
			return dists;
		}

		//Find the partitions covered by a radius around a specific point
		// the point is specified by its distance from each vantage point
		std::vector<int64_t> find_parts_(const std::vector<int32_t>& dists, int32_t radius)
		{
			//get the shell bounds for each vantage point from the mvp_parts table
			std::vector<std::vector<int32_t>> bounds;
			int64_t prev_vp_id = -1;
			sel_parts->reset();
			while (sel_parts->executeStep()) {
				int64_t vp_id = sel_parts->getColumn(0).getInt64();
				int32_t upper_bound = sel_parts->getColumn(1).getInt();
				if (vp_id != prev_vp_id) {
					//handling a new vantage point
					bounds.push_back(std::vector<int32_t>());
					bounds.back().push_back(0); // 0 is the lowest bound -- this simplifies later logic
				}
				bounds.back().push_back(upper_bound);
			}

			return parts;
		}

		// Insert a point into points, if it doesn't already exist, returning the id
		// No transaction
		int64_t insert_point_(const point_type& p_value)
		{
			//is the point already in the database?
			sel_point_by_value->bind("value", get_point_data(p_value), get_point_size(p_value));
			sel_point_by_value->reset();
			if (sel_point_by_value->executeStep()) {
				//yes
				return sel_point_by_value->getColumn(0).getInt64();
			}
			else {
				//no: we need to add the point

				auto vp_dists = vp_dists_(p_value);

				//update the insert_point statement if vp_ids changed
				if (!insert_point) insert_point = make_insert_point(vp_ids_);

				insert_point->bind(1, get_point_data(p_value), get_point_size(p_value));
				for (size_t i = 0; i < dists.size(); ++i) {
					insert_point->bind(i + 2, dists[i]); //the first parameter has index 1, so these start at 2
				}
				insert_point->reset();
				if (insert_point->executeStep()) {
					//increment the point count
					increment_count->bind("col", "points");
					increment_count->reset();
					increment_count->exec();

					return insert_point->getColumn(0).getInt64();
				}
				else {
					throw std::runtime_error("Error inserting point");
				}
			}
		}

		// Insert an item, updates the point_id if the item already exists
		// No transaction
		void insert_item_(int64_t point_id, const item_type& item)
		{
			//is the item already in the database?
			get_file_by_path->bind("path", item);
			get_file_by_path->reset();
			if (get_file_by_path->executeStep()) {
				//yes, does the pointid match?
				auto old_id = get_file_by_path->getColumn(0).getInt64();
				if (old_id != point_id) {
					//no -- update it
					update_file->bind("path", item);
					update_file->bind("pointid", point_id);
					update_file->reset();
					update_file->exec();
				}
			}
			else {
				//no: we need to insert the file
				insert_file->bind("path", item);
				insert_file->bind("pointid", point_id);
				insert_file->reset();
				insert_file->exec();

				increment_count->bind("col", "files");
				increment_count->reset();
				increment_count->exec();
			}
		}

		// Insert a vantage point into points, throws an exception if it already exists
		// No transaction
		int64_t insert_vantage_point_(const point_type& vp_value)
		{
			insert_vantage_point->bind("value", get_point_data(vp_value), get_point_size(vp_value));
			insert_vantage_point->reset();
			if (insert_vantage_point->executeStep()) {
				increment_count->bind("col", "vantage_points");
				increment_count->reset();
				increment_count->exec();

				return insert_vantage_point->getColumn(0).getInt64();
			}
			else {
				throw std::runtime_error("Error inserting new vantage_point");
			}
		}

		// Add a new column to points for the given vantage_point
		//  The new column is named "d{vp_id}" and will be populated by get_distance(vp_value, points.value)
		// No transaction
		void add_points_column_(int64_t vp_id, const point_type& vp_value)
		{
			std::string col_name = "d" + std::to_string(vp_id);

			add_points_col->bind("col", col_name);
			add_points_col->bind("idx", "idx_" + col_name);
			add_points_col->reset();
			add_points_col->exec();

			// we need to compute the distance from the new vantage point to all of the existing points
			update_point->bind("col", col_name);

			get_all_point_values->reset();
			while (get_all_point_values->executeStep()) {
				auto id = get_all_point_values->getColumn(0).getInt64();
				auto p_value = get_point_value(get_all_point_values->getColumn(1));

				uint32_t d = get_distance(vp_value, p_value);
				update_point->bind("id", id);
				update_point->bind("val", d);
				update_point->reset();
				update_point->exec();
			}
		}

	public:
		// Construct, open or create the database
		Impl(const std::string& path)
			: db(path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE)
		{
			//initialize database as necessary
			init_tables();

			//precompile statements
			upd_count = make_stmt(str_upd_count);
			
			sel_all_points = make_stmt(str_sel_all_points);
			sel_point_by_value = make_stmt(str_sel_point_by_value);
			upd_point = make_stmt(str_upd_point);
			add_points_col = make_stmt(str_add_points_col);
			
			sel_vps = make_stmt(str_sel_vps);
			ins_vp = make_stmt(str_ins_vp);
			
			sel_parts = make_stmt(str_sel_parts);
			del_query = make_stmt(str_del_query);
			ins_query = make_stmt(str_ins_query);

			ins_item = make_stmt(str_ins_item);
			upd_item = make_stmt(str_upd_item);
			sel_item_by_id = make_stmt(str_sel_item_by_id);
		}

		
		// Insert a (point, item) pair
		//  Multiple items may be associated with the same point
		//  If the point is new, its distance to all existing vantage points will be computed and stored
		void insert(const point_type& p_value, const item_type& item)
		{
			SQLite::Transaction trans(db);
			auto point_id = insert_point_(p_value);
			insert_item_(point_id, item);
			trans.commit();
		}

		void add_vantage_point(const point_type& vp_value)
		{
			SQLite::Transaction trans(db);
			
			auto vp_id = insert_vantage_point_(vp_value);
			add_points_column_(vp_id, vp_value);

			trans.commit();
		}
	
		std::vector<item_type> query(const point_type& pt, unsigned int radius, int64_t limit)
		{
			//we need the distance from each vantage point to pt
			SQLite::Transaction trans(db);

			std::vector<int64_t> vp_ids;
			std::vector<uint32_t> lower_bounds, upper_bounds;
			get_vantage_points->reset();
			while (get_vantage_points->executeStep()) {
				auto id = get_vantage_points->getColumn(0).getInt64(); //vantage point id
				vp_ids.push_back(id);

				auto vp_value = get_point_value(get_vantage_points->getColumn(1)); //vantage point value
				//get the distance
				uint32_t d = get_distance(vp_value, pt);
				lower_bounds.push_back(d <= radius ? 0 : d - radius);
				upper_bounds.push_back(d >= 0xFFFFFFFF - radius ? 0xFFFFFFFF : d + radius);
			}
			update_vp_ids(vp_ids);
			//we get all of the point within the lower and upper bounds using parition_points
			// for each, we compute the distance from the query point to it, and store
			// the result in the temp.query table
			clear_query->reset();
			clear_query->exec();
			
			if (!partition_points) partition_points = make_partition_points(vp_ids);
			
			for (size_t i = 0; i < vp_ids.size(); ++i) {
				partition_points->bind(2 * i + 1, lower_bounds[i]);
				partition_points->bind(2 * i + 2, upper_bounds[i]);
			}
		
			partition_points->reset();
			while (partition_points->executeStep()) {
				auto id = partition_points->getColumn(0).getInt64();
				auto value = get_point_value(partition_points->getColumn(1));

				uint32_t d = get_distance(pt, value);

				insert_query->bind("id", id);
				insert_query->bind("dist", d);
				insert_query->reset();
				insert_query->exec();
			}

			std::vector<item_type> result;
			get_files_by_query->bind("limit", limit);
			get_files_by_query->reset();
			while (get_files_by_query->executeStep()) {
				result.push_back(get_item(get_files_by_query->getColumn(0)));
			}

			trans.commit();

			return result;
		}

		point_type find_vantage_point(size_t sample_size)
		{
			SQLite::Transaction trans(db);

			// do we have any vantage points yet?
			int64_t num_vantage_points = -1;
			get_count->reset();
			if (get_count->executeStep()) {
				num_vantage_points = get_count->getColumn(1).getInt64();
			}
			else {
				throw std::runtime_error("Error reading number of vantage points");
			}

			if (num_vantage_points > 0) {
				//we need to find a point that's far from all of the existing vantage points

			}
			else {
				//we need to find a point that's far from all other points
			}

			trans.commit();
		}
	};

	//Open the database
	Database::Database(const std::string& path)
		: impl(std::make_unique<Impl>(path))
	{
		//nothing else to do
	}

	//Close the database
	Database::~Database()
	{

	}

	//Add a file
	void Database::insert(const point_type& point, const item_type& item)
	{
		impl->insert(point, item);
	}

	//Find similar images
	std::vector<Database::item_type> Database::query(const point_type& point, unsigned int dist, size_t limit)
	{
		return impl->query(point, dist, static_cast<int64_t>(limit));
	}

	//Add a vantage point for querying
	void Database::add_vantage_point(const point_type& point)
	{
		impl->add_vantage_point(point);
	}

	//Find a point that would make a good vantage point
	Database::point_type Database::find_vantage_point(size_t sample_size)
	{
		return impl->find_vantage_point(sample_size);
	}
	
}