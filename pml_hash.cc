#include "pml_hash.h"
/**
 * PMLHash::PMLHash 
 * 
 * @param  {char*} file_path : the file path of data file
 * if the data file exist, open it and recover the hash
 * if the data file does not exist, create it and initial the hash
 */
PMLHash::PMLHash(const char* file_path) {
    char *pmemaddr;
	size_t mapped_len;
	int is_pmem;

    // 这个函数将目标文件通过内存映射的方式打开
	if ((pmemaddr = pmem_map_file(file_path, FILE_SIZE, PMEM_FILE_CREATE,
			0666, &mapped_len, &is_pmem)) == NULL) {
		perror("pmem_map_file");
		exit(1);
	}

    this->start_addr = pmemaddr;
    void* meta_buf = reinterpret_cast<void*>(pmemaddr);
    this->meta = new(meta_buf) metadata;
	this->meta->size = TABLE_SIZE;

    void* table_buf = reinterpret_cast<void*>(pmemaddr+1024); // 给meta数据安排1KB的内存
    this->table_arr = new(table_buf) pm_table;
    this->overflow_addr = this->table_arr+1024*1024*8-1024;
}
/**
 * PMLHash::~PMLHash 
 * 
 * unmap and close the data file
 */
PMLHash::~PMLHash() {
    // 这个函数用于关闭pmem_map打开的NVM文件
    pmem_unmap(start_addr, FILE_SIZE);
}
/**
 * PMLHash 
 * 
 * split the hash table indexed by the meta->next
 * update the metadata
 */
void PMLHash::split() {
    // fill the split table
    // fill the new table
    int size = this->meta->size;
    // 在原数组后面加上一个新桶
    void* newBuf = reinterpret_cast<void*>(table_arr+size*sizeof(pm_table));
    auto newTable = new(newBuf) pm_table;

    auto oldTable = this->table_arr[this->meta->next];
    auto oldTablePos = 0, newTablePos = 0;
    for (auto i = 0; i < oldTable.fill_num; i++) {
        if (oldTable.kv_arr[i]/this->meta->size % 2 == 1) {
            newTable.kv_arr[newTablePos++] = oldTable.kv_arr[i];
        } else {
            oldTable.kv_arr[oldTablePos++] = oldTable.kv_arr[i];
        }
    }
    this->meta->next++;
    this->meta->size++;
    if (this->meta->next == this->meta->size) {
        this->meta->next = 0;
        this->meta->level++;
    }

    pmem_persist(start_addr, FILE_SIZE); // 这个函数调用CLFLUSH和SFENCE指令来显式持久化相应的数据
    return;
}
/**
 * PMLHash 
 * 
 * @param  {uint64_t} key     : key
 * @param  {size_t} hash_size : the N in hash func: idx = hash % N
 * @return {uint64_t}         : index of hash table array
 * 
 * need to hash the key with proper hash function first
 * then calculate the index by N module
 */
uint64_t PMLHash::hashFunc(const uint64_t &key, const size_t &hash_size) {
    uint64_t PMLHash::hashFunc(const uint64_t &key, const size_t &hash_size) {
    uint64_t pos = key % (hash_size*math.pow(2,this->meta->level)); // 取模公式
    if (pos < next) {
        pos = key % (hash_size*math.pow(2,this->meta->level+1)); // 该节点以及分裂过了
    }
    return pos;
}
}

/**
 * PMLHash 
 * 
 * @param  {uint64_t} offset : the file address offset of the overflow hash table
 *                             to the start of the whole file
 * @return {pm_table*}       : the virtual address of new overflow hash table
 */
pm_table* PMLHash::newOverflowTable(uint64_t &offset) {
    // 再偏移值后面新建一个和原数据表一样大小的数据表
    auto addr = this->start_addr+offset
    void* newBuf = reinterpret_cast<void*>(table_arr+size*sizeof(pm_table));
    auto newTable = new(newBuf) pm_table[this->meta->size];
    return addr;
}

/**
 * PMLHash 
 * 
 * @param  {uint64_t} key   : inserted key
 * @param  {uint64_t} value : inserted value
 * @return {int}            : success: 0. fail: -1
 * 
 * insert the new kv pair in the hash
 * 
 * always insert the entry in the first empty slot
 * 
 * if the hash table is full then split is triggered
 */
int PMLHash::insert(const uint64_t &key, const uint64_t &value) {
    auto newEntry = new entry(key, value);
    auto pos = hashFunc(key, HASH_SIZE);
    int next = this->meta->next;
    if (this->table_arr->fill_num == TABLE_SIZE) { // 判断是否需要分裂
        if (next != pos) {  // 不是分裂这个节点，overflow
            int newPos = this->overflow_addr[pos].fill_num;
            this->overflow_addr[pos].kv_arr[newPos] = *newEntry;
            this->overflow_addr[pos].fill_num++;
            this->meta->overflow_num++;
            pmem_persist(start_addr, FILE_SIZE);
            return 0;
        } else {  // 分裂后直接判断需要插入哪个节点
            split();
            pos = hashFunc(key, HASH_SIZE);
            int newPos = this->table_arr[pos].fill_num;
            this->table_arr[pos].kv_arr[newPos] = *newEntry;
            this->table_arr[pos].fill_num++;
            pmem_persist(start_addr, FILE_SIZE);
            return 0;
        }
    } else {
        // 直接插入指定位置
        int newPos = this->table_arr[pos].fill_num;
        this->table_arr[pos].kv_arr[newPos] = *newEntry;
        this->table_arr[pos].fill_num++;
        pmem_persist(start_addr, FILE_SIZE);
        return 0;
    }
    return -1;
}

/**
 * PMLHash 
 * 
 * @param  {uint64_t} key   : the searched key
 * @param  {uint64_t} value : return value if found
 * @return {int}            : 0 found, -1 not found
 * 
 * search the target entry and return the value
 */
int PMLHash::search(const uint64_t &key, uint64_t &value) {
    auto pos = hashFunc(key);
    auto table = this->table_arr[pos]; // 判断溢出表中是否存在
    auto len = table.fill_num;
    for (auto i = 0; i < len; i++) { // 顺序查找
        if (table.kv_arr[i].key == key && table.kv_arr[i].value == value) {
            
            return 0;
        }
    }
    if (len == TABLE_SIZE) { // 判断溢出表中是否存在
        auto table = this->overflow_addr[pos];
        auto len = table.fill_num;
        for (auto i = 0; i < len; i++) {
            if (table.kv_arr[i].key == key && table.kv_arr[i].value == value) {
                return 0;
            }
        }
    }
    return -1;
}

/**
 * PMLHash 
 * 
 * @param  {uint64_t} key : target key
 * @return {int}          : success: 0. fail: -1
 * 
 * remove the target entry, move entries after forward
 * if the overflow table is empty, remove it from hash
 */
int PMLHash::remove(const uint64_t &key) {
    auto pos = hashFunc(key);
    auto table = &(this->table_arr[pos]);
    auto len = table->fill_num;
    for (auto i = 0; i < len; i++) {  // 先判断原始哈希表中是否存在
        if (table->kv_arr[i].key == key) {
            for (auto j = i+1; j < len-1; j++) {  // 将后面的数往前移动
                table->kv_arr[j] = table->kv_arr[j+1];
            }
            table->fill_num--;
            return 0;
        }
    }
    if (len == TABLE_SIZE) { // 判断溢出表中是否存在
        auto table = &(this->overflow_addr[pos]);
        auto len = table->fill_num;
        for (auto i = 0; i < len; i++) {
            if (table->kv_arr[i].key == key) {
                for (auto j = i+1; j < len-1; j++) {  // 将后面的数往前移动
                    table->kv_arr[j] = table->kv_arr[j+1];
                }
                table->fill_num--;
                this->meta->overflow_num--;
                return 0;
            }
        }
    }
    return -1;
}

/**
 * PMLHash 
 * 
 * @param  {uint64_t} key   : target key
 * @param  {uint64_t} value : new value
 * @return {int}            : success: 0. fail: -1
 * 
 * update an existing entry
 */
int PMLHash::update(const uint64_t &key, const uint64_t &value) {
    auto pos = hashFunc(key);
    auto table = &(this->table_arr[pos]);
    auto len = table->fill_num;
    for (auto i = 0; i < len; i++) {
        if (table->kv_arr[i].key == key) {
            table->kv_arr[i].value = value; // 修改value
            return 0;
        }
    }
    if (len == TABLE_SIZE) { // 判断溢出表中是否存在
        auto table = &(this->overflow_addr[pos]);
        auto len = table->fill_num;
        for (auto i = 0; i < len; i++) {
            if (table->kv_arr[i].key == key) {
                table->kv_arr[i].value = value; // 修改value
                return 0;
            }
        }
    }
    return -1;
}