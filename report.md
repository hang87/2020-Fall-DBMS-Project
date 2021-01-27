[TOC]

#### 小组信息

小组组长：15331257 钱航

小组人数：1人

#### 基于Git进行团队开发 

项目地址：https://github.com/hang87/2020-Fall-DBMS-Project

#### 模拟NVM环境和相应截图

模拟NVM环境的代码在testNVM/helloworld文件夹中，测试代码借鉴github上的[项目](https://github.com/pmem/pmdk-examples)

##### 写内容

```c++
/****************************
 * This function writes the "Hello..." string to persistent-memory.
 *****************************/
void write_hello_string (char *buf, char *path)
{
	char *pmemaddr;
	size_t mapped_len;
	int is_pmem;
	
	/* create a pmem file and memory map it */
	if ((pmemaddr = (char *)pmem_map_file(path, PMEM_LEN, PMEM_FILE_CREATE,
				0666, &mapped_len, &is_pmem)) == NULL) {
		perror("pmem_map_file");
		exit(1);
	}
	/* store a string to the persistent memory */
	strcpy(pmemaddr, buf);

	/* flush above strcpy to persistence */
	if (is_pmem)
		pmem_persist(pmemaddr, mapped_len);
	else
		pmem_msync(pmemaddr, mapped_len);

	/* output a string from the persistent memory to console */
	printf("\nWrite the (%s) string to persistent memory.\n",pmemaddr);	
			
	return;	
}
```

##### 读内容

```c++
/****************************
 * This function reads the "Hello..." string from persistent-memory.
 *****************************/
void read_hello_string(char *path)
{
	char *pmemaddr;
	size_t mapped_len;
	int is_pmem;

		/* open the pmem file to read back the data */
		if ((pmemaddr = (char *)pmem_map_file(path, PMEM_LEN, PMEM_FILE_CREATE,
					0666, &mapped_len, &is_pmem)) == NULL) {
			perror("pmem_map_file");
			exit(1);
		}  	
		/* Reading the string from persistent-memory and write to console */
		printf("\nRead the (%s) string from persistent memory.\n",pmemaddr);

	return;
}
```

##### 运行截图

![avatar](./testNVM/hello_libpmem.png)

##### 配置过程截图

![avatar](./testNVM/pmdk.png)

#### 线性哈希的增删改查功能实现

##### 创建线性哈希表

首先用pmem_map_file函数将目标文件通过内存映射的方式打开，然后给meta数据安排1KB的内存，剩下的8MB-1KB的内存留给原始表，剩下8MB留给溢出表。

```c++
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
```



##### 哈希函数

哈希函数即找key对应的桶的位置。首先取模公式为 key % hash_size*2^level^ .如果得到的位置小于分裂点，则说明该节点以及分裂过了，需要取level+1再重新计算过。

```c++
uint64_t PMLHash::hashFunc(const uint64_t &key, const size_t &hash_size) {
    uint64_t pos = key % (hash_size*math.pow(2,this->meta->level)); // 取模公式
    if (pos < next) {
        pos = key % (hash_size*math.pow(2,this->meta->level+1)); // 该节点以及分裂过了
    }
    return pos;
}
```



##### 分裂

分裂操作分为几步

1. 新建新的哈希桶。是在原数组后面加上一个新桶，具体操作是在根据NVM编程特点，在地址table_arr上找到原数组的偏移量，在算出的地址新建一个哈希桶
2. 分裂旧哈希桶。即判断旧哈希桶中的每个数据项模原来的值的2倍，根据取余值确定该数据项在旧桶还是在新桶。
3. meta值修改。next值下移，桶数量加一，如果next值到了取模数，则取模数翻倍，即level加一，next值重置为0。

分裂节点后，调用pmem_persist函数持久化修改操作。

```c++
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
```



##### 增加数据项

增加数据项分3种情况，哈希桶有空位，哈希桶满了但是该哈希桶不是分裂节点，哈希桶满了且该哈希桶是分裂节点。

1. 如果有空位则直接插入到最靠前的空槽中。
2. 哈希桶满了，将新的数据放到溢出表的对应哈希桶中。首先根据overflow_addr地址找到对应要插入的溢出表的位置，然后插入到溢出表对应哈希桶最靠前的溢出表中。
3. 哈希桶满了，先分裂该节点，然后再判断新插入的要插入的位置，以方式1插入对应哈希桶中。

插入数据项后，调用pmem_persist函数持久化修改操作。

```c++
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
            int newPos = this->start_addr[pos].fill_num;
            this->start_addr[pos].kv_arr[newPos] = *newEntry;
            this->start_addr[pos].fill_num++;
            pmem_persist(start_addr, FILE_SIZE);
            return 0;
        }
    } else {
        // 直接插入指定位置
        int newPos = this->start_addr[pos].fill_num;
        this->start_addr[pos].kv_arr[newPos] = *newEntry;
        this->start_addr[pos].fill_num++;
        pmem_persist(start_addr, FILE_SIZE);
        return 0;
    }
    return -1;
}

```



##### 删除数据项

删除分2种情况，在原始桶和哈希桶的位置查找，如果找到就删除。删除的操作是将后面的哈希桶数据项往前移动，然后fill_num减一。

删除数据项后，调用pmem_persist函数持久化修改操作。

```c++
int PMLHash::remove(const uint64_t &key) {
    auto pos = hashFunc(key);
    auto table = &(this->start_addr[pos]);
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
```



##### 改动数据项

改动分2种情况，在原始桶和哈希桶的位置查找，如果找到就修改value值，并返回-1。否则就返回-1。

改动数据项后，调用pmem_persist函数持久化修改操作。

```c++
int PMLHash::update(const uint64_t &key, const uint64_t &value) {
    auto pos = hashFunc(key);
    auto table = &(this->start_addr[pos]);
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
```



##### 查找数据项

查找分2种情况，在原始桶和哈希桶的位置查找。先在原始哈希表中找到对应的哈希桶，然后顺序遍历哈希桶中的数据项，如果找到就返回0，否则在对应的溢出表中查找，步骤和原始表一样。

```c++
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
```

#### 参考

1. https://github.com/pmem/pmdk-examples/tree/master/hello_world/libpmem
2. https://blog.csdn.net/SweeNeil/article/details/90265226
3. https://github.com/ZhangJiaQiao/2020-Fall-DBMS-Project
4. https://www.it1352.com/351047.html
5. https://blog.csdn.net/jackydai987/article/details/6673063

