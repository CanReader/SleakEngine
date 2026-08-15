#ifndef _TREE_H_
#define _TREE_H_

namespace Sleak
{
    /**
 * @class Tree
 * @brief Implements a binary tree data structure for hierarchical data organization.
 *
 * The BinaryTree class organizes data in a hierarchical manner with nodes and edges, where
 * each node has at most two children. It is suitable for scenarios where you need to
 * represent hierarchical relationships, such as file systems, decision trees, or
 * expression trees. Use this class when you need to implement tree-based algorithms,
 * search trees, or hierarchical data storage.
 *
 * Example Use Cases:
 * - Representing a file system directory structure.
 * - Implementing decision trees in machine learning.
 * - Storing and evaluating arithmetic expressions.
 * - Implementing binary search trees for efficient searching.
 *
 * Implementation Details:
 * - Uses a tree node structure with pointers to left and right children.
 * - Provides methods for inserting, searching, and traversing nodes.
 * - Supports various tree traversal algorithms (e.g., in-order, pre-order, post-order).
 * @ingroup utility
 */
    template <typename T>
    class Tree
    {
    private:
        /// One binary search tree node.
        struct Node
        {
            T data;
            Node* left;
            Node* right;

            Node(const T& value) : data(value), left(nullptr), right(nullptr) {}
        };

        Node* root;

        /// Inserts value under node per BST ordering, returning the (possibly new) subtree root.
        Node* insertRecursive(Node* node, const T& value)
        {
            if (!node)
                return new Node(value);

            if (value < node->data)
                node->left = insertRecursive(node->left, value);
            else
                node->right = insertRecursive(node->right, value);

            return node;
        }

        /// Leftmost (minimum) node in the subtree rooted at node.
        Node* findMin(Node* node)
        {
            while (node->left)
                node = node->left;
            return node;
        }

        /// Removes value from the subtree rooted at node, returning the (possibly new) subtree root.
        Node* removeRecursive(Node* node, const T& value)
        {
            if (!node)
                return nullptr;

            if (value < node->data)
                node->left = removeRecursive(node->left, value);
            else if (value > node->data)
                node->right = removeRecursive(node->right, value);
            else
            {
                if (!node->left)
                {
                    Node* temp = node->right;
                    delete node;
                    return temp;
                }
                else if (!node->right)
                {
                    Node* temp = node->left;
                    delete node;
                    return temp;
                }

                Node* temp = findMin(node->right);
                node->data = temp->data;
                node->right = removeRecursive(node->right, temp->data);
            }
            return node;
        }

        /// In-order traversal of the subtree rooted at node.
        void inOrderRecursive(Node* node)
        {
            if (!node)
                return;
            inOrderRecursive(node->left);
            // Print function can be added here
            inOrderRecursive(node->right);
        }

        /// Deletes every node in the subtree rooted at node.
        void clearRecursive(Node* node)
        {
            if (!node)
                return;
            clearRecursive(node->left);
            clearRecursive(node->right);
            delete node;
        }

    public:
        Tree() : root(nullptr) {}

        /// Deletes every node in the tree.
        ~Tree()
        {
            clearRecursive(root);
        }

        /// Inserts value, keeping BST ordering (duplicates go right).
        void insert(const T& value)
        {
            root = insertRecursive(root, value);
        }

        /// Removes value from the tree, if present.
        void remove(const T& value)
        {
            root = removeRecursive(root, value);
        }

        /// True if value exists in the subtree rooted at node.
        bool contains(Node* node, const T& value)
        {
            if (!node)
                return false;
            if (node->data == value)
                return true;
            return contains(value < node->data ? node->left : node->right, value);
        }

        /// True if value exists anywhere in the tree.
        bool contains(const T& value)
        {
            return contains(root, value);
        }

        /// Removes every node, leaving an empty tree.
        void clear()
        {
            clearRecursive(root);
            root = nullptr;
        }

        /// Traverses the tree in-order (left, node, right).
        void inOrder()
        {
            inOrderRecursive(root);
        }
    };
}

#endif // _BINARY_TREE_H_
